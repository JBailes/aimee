/* agent_tools.c: tool execution (bash, read_file, write_file, list_files, verify, git_log),
 * checkpoints */
#include "aimee.h"
#include "agent_tools.h"
#include "agent_exec.h"
#include "workspace.h"
#include "cJSON.h"
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef _WIN32
#include <glob.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <fcntl.h>
#include <poll.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <sys/wait.h>
#endif

/* --- Execution Transactions (Feature 3) --- */

int exec_checkpoint_capture(exec_checkpoint_t *cp, const char *path)
{
   if (!cp || !path)
      return -1;
   memset(cp, 0, sizeof(*cp));
   snprintf(cp->path, sizeof(cp->path), "%s", path);

   FILE *f = fopen(path, "r");
   if (!f)
   {
      cp->original_content = NULL; /* file didn't exist */
      return 0;
   }
   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (sz < 0 || sz > MAX_FILE_SIZE)
   {
      fclose(f);
      return -1;
   }
   if (sz == 0)
   {
      cp->original_content = safe_strdup("");
      fclose(f);
      return 0;
   }
   cp->original_content = malloc((size_t)sz + 1);
   if (!cp->original_content)
   {
      fclose(f);
      return -1;
   }
   size_t nread = fread(cp->original_content, 1, (size_t)sz, f);
   if (ferror(f) || (long)nread != sz)
   {
      free(cp->original_content);
      cp->original_content = NULL;
      fclose(f);
      return -1;
   }
   cp->original_content[nread] = '\0';
   fclose(f);
   return 0;
}

int exec_checkpoint_restore(const exec_checkpoint_t *cp)
{
   if (!cp || !cp->path[0])
      return -1;
   if (!cp->original_content)
   {
      /* File didn't exist before, remove it */
      unlink(cp->path);
      return 0;
   }
   FILE *f = fopen(cp->path, "wb");
   if (!f)
      return -1;
   size_t len = strlen(cp->original_content);
   size_t written = fwrite(cp->original_content, 1, len, f);
   if (fclose(f) != 0 || written != len)
      return -1;
   return 0;
}

void exec_checkpoint_free(exec_checkpoint_t *cp)
{
   if (cp)
   {
      free(cp->original_content);
      cp->original_content = NULL;
   }
}

/* --- Per-invocation checkpoint context --- */

checkpoint_ctx_t *checkpoint_ctx_new(void)
{
   checkpoint_ctx_t *ctx = calloc(1, sizeof(*ctx));
   return ctx;
}

void checkpoint_ctx_push(checkpoint_ctx_t *ctx, const char *path)
{
   if (!ctx || ctx->count >= AGENT_MAX_CHECKPOINTS)
      return;
   if (exec_checkpoint_capture(&ctx->checkpoints[ctx->count], path) == 0)
      ctx->count++;
}

void checkpoint_ctx_rollback_all(checkpoint_ctx_t *ctx)
{
   if (!ctx)
      return;
   for (int i = ctx->count - 1; i >= 0; i--)
   {
      if (!ctx->checkpoints[i].rolled_back)
      {
         exec_checkpoint_restore(&ctx->checkpoints[i]);
         ctx->checkpoints[i].rolled_back = 1;
      }
   }
}

void checkpoint_ctx_clear(checkpoint_ctx_t *ctx)
{
   if (!ctx)
      return;
   for (int i = 0; i < ctx->count; i++)
      exec_checkpoint_free(&ctx->checkpoints[i]);
   ctx->count = 0;
}

void checkpoint_ctx_free(checkpoint_ctx_t *ctx)
{
   if (!ctx)
      return;
   checkpoint_ctx_clear(ctx);
   free(ctx);
}

/* Legacy global-state shims (for callers not yet migrated) */
static checkpoint_ctx_t g_compat_ctx;

void exec_checkpoint_push(const char *path)
{
   checkpoint_ctx_push(&g_compat_ctx, path);
}

void exec_checkpoints_rollback_all(void)
{
   checkpoint_ctx_rollback_all(&g_compat_ctx);
}

void exec_checkpoints_clear(void)
{
   checkpoint_ctx_clear(&g_compat_ctx);
}

/* --- Tool execution (Unix only) --- */

#ifndef _WIN32

char *tool_bash(const char *command, int timeout_ms)
{
   int stdout_pipe[2], stderr_pipe[2];
   if (pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0)
      return safe_strdup("{\"stdout\":\"\",\"stderr\":\"pipe failed\",\"exit_code\":-1}");

   pid_t pid = fork();
   if (pid < 0)
   {
      close(stdout_pipe[0]);
      close(stdout_pipe[1]);
      close(stderr_pipe[0]);
      close(stderr_pipe[1]);
      return safe_strdup("{\"stdout\":\"\",\"stderr\":\"fork failed\",\"exit_code\":-1}");
   }

   if (pid == 0)
   {
      /* Child */
      close(stdout_pipe[0]);
      close(stderr_pipe[0]);
      dup2(stdout_pipe[1], STDOUT_FILENO);
      dup2(stderr_pipe[1], STDERR_FILENO);
      close(stdout_pipe[1]);
      close(stderr_pipe[1]);
      execl("/bin/sh", "sh", "-c", command, (char *)NULL);
      _exit(127);
   }

   /* Parent */
   close(stdout_pipe[1]);
   close(stderr_pipe[1]);

   char *out_buf = malloc(AGENT_TOOL_OUTPUT_RAW_MAX + 1);
   char *err_buf = malloc(AGENT_TOOL_OUTPUT_RAW_MAX + 1);
   if (!out_buf || !err_buf)
   {
      free(out_buf);
      free(err_buf);
      close(stdout_pipe[0]);
      close(stderr_pipe[0]);
      kill(pid, SIGKILL);
      waitpid(pid, NULL, 0);
      return safe_strdup("error: out of memory");
   }
   size_t out_len = 0, err_len = 0;
   int timed_out = 0;

   struct timespec deadline;
   clock_gettime(CLOCK_MONOTONIC, &deadline);
   deadline.tv_sec += timeout_ms / 1000;
   deadline.tv_nsec += (timeout_ms % 1000) * 1000000L;
   if (deadline.tv_nsec >= 1000000000L)
   {
      deadline.tv_sec++;
      deadline.tv_nsec -= 1000000000L;
   }

   int max_fd = (stdout_pipe[0] > stderr_pipe[0] ? stdout_pipe[0] : stderr_pipe[0]) + 1;
   int stdout_open = 1, stderr_open = 1;

   /* Guard against FD_SET overflow if pipe fds exceed FD_SETSIZE */
   if (stdout_pipe[0] >= FD_SETSIZE || stderr_pipe[0] >= FD_SETSIZE)
   {
      close(stdout_pipe[0]);
      close(stderr_pipe[0]);
      kill(pid, SIGKILL);
      waitpid(pid, NULL, 0);
      free(out_buf);
      free(err_buf);
      return safe_strdup("{\"stdout\":\"\",\"stderr\":\"fd exceeds FD_SETSIZE\",\"exit_code\":-1}");
   }

   while (stdout_open || stderr_open)
   {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      long remain_ms =
          (deadline.tv_sec - now.tv_sec) * 1000 + (deadline.tv_nsec - now.tv_nsec) / 1000000;
      if (remain_ms <= 0)
      {
         timed_out = 1;
         break;
      }

      fd_set rfds;
      FD_ZERO(&rfds);
      if (stdout_open)
         FD_SET(stdout_pipe[0], &rfds);
      if (stderr_open)
         FD_SET(stderr_pipe[0], &rfds);

      struct timeval tv;
      tv.tv_sec = remain_ms / 1000;
      tv.tv_usec = (remain_ms % 1000) * 1000;
      int sel = select(max_fd, &rfds, NULL, NULL, &tv);
      if (sel <= 0)
      {
         timed_out = 1;
         break;
      }

      if (stdout_open && FD_ISSET(stdout_pipe[0], &rfds))
      {
         ssize_t n = read(stdout_pipe[0], out_buf + out_len, AGENT_TOOL_OUTPUT_RAW_MAX - out_len);
         if (n <= 0)
            stdout_open = 0;
         else
            out_len += (size_t)n;
      }
      if (stderr_open && FD_ISSET(stderr_pipe[0], &rfds))
      {
         ssize_t n = read(stderr_pipe[0], err_buf + err_len, AGENT_TOOL_OUTPUT_RAW_MAX - err_len);
         if (n <= 0)
            stderr_open = 0;
         else
            err_len += (size_t)n;
      }
      if (out_len >= AGENT_TOOL_OUTPUT_RAW_MAX)
      {
         close(stdout_pipe[0]);
         stdout_pipe[0] = -1;
         stdout_open = 0;
      }
      if (err_len >= AGENT_TOOL_OUTPUT_RAW_MAX)
      {
         close(stderr_pipe[0]);
         stderr_pipe[0] = -1;
         stderr_open = 0;
      }
   }

   if (stdout_pipe[0] >= 0)
      close(stdout_pipe[0]);
   if (stderr_pipe[0] >= 0)
      close(stderr_pipe[0]);

   int exit_code = -1;
   if (timed_out)
   {
      kill(pid, SIGTERM);
      usleep(100000);
      kill(pid, SIGKILL);
      waitpid(pid, NULL, 0);
      exit_code = -1;
   }
   else
   {
      int status = 0;
      waitpid(pid, &status, 0);
      if (WIFEXITED(status))
         exit_code = WEXITSTATUS(status);
   }

   out_buf[out_len] = '\0';
   err_buf[err_len] = '\0';

   /* Compress output to fit token budget (#4) */
   char *compressed_out = agent_compress_tool_result(out_buf, out_len);
   char *compressed_err = agent_compress_tool_result(err_buf, err_len);

   /* Build JSON result */
   cJSON *result = cJSON_CreateObject();
   cJSON_AddStringToObject(result, "stdout", compressed_out);
   cJSON_AddStringToObject(result, "stderr", compressed_err);
   cJSON_AddNumberToObject(result, "exit_code", exit_code);
   char *json = cJSON_PrintUnformatted(result);
   cJSON_Delete(result);

   free(compressed_out);
   free(compressed_err);
   free(out_buf);
   free(err_buf);
   return json;
}

/* Validate a file path: delegates to the shared guardrail-level check. */
static const char *validate_file_path(const char *path, char *resolved, size_t resolved_len)
{
   return guardrails_validate_file_path(path, resolved, resolved_len);
}

char *tool_read_file(const char *path, int offset, int limit)
{
   char *resolved_proposal = NULL;
   const char *actual_path = path;

   if (strncmp(path, "proposal:", 9) == 0)
   {
      resolved_proposal = resolve_proposal_path(path + 9);
      if (resolved_proposal)
         actual_path = resolved_proposal;
      else
         actual_path = path + 9; /* try as-is even if resolve failed */
   }

   char resolved[MAX_PATH_LEN];
   const char *err = validate_file_path(actual_path, resolved, sizeof(resolved));
   if (err)
   {
      if (resolved_proposal)
         free(resolved_proposal);
      return safe_strdup(err);
   }

   FILE *f = fopen(actual_path, "r");
   if (!f)
   {
      char err_msg[512];
      snprintf(err_msg, sizeof(err_msg), "error: cannot open %s", actual_path);
      if (resolved_proposal)
         free(resolved_proposal);
      return safe_strdup(err_msg);
   }

   if (resolved_proposal)
      free(resolved_proposal);

   char *buf = malloc(AGENT_TOOL_OUTPUT_MAX + 1);
   if (!buf)
   {
      fclose(f);
      return safe_strdup("error: out of memory");
   }
   size_t total = 0;
   char line[4096];
   int line_num = 0;
   int lines_read = 0;
   int max_lines = (limit > 0) ? limit : 100000;

   while (fgets(line, sizeof(line), f))
   {
      line_num++;
      if (offset > 0 && line_num <= offset)
         continue;
      size_t len = strlen(line);
      if (total + len >= AGENT_TOOL_OUTPUT_MAX)
      {
         size_t avail = AGENT_TOOL_OUTPUT_MAX - total;
         if (avail > 0)
            memcpy(buf + total, line, avail);
         total = AGENT_TOOL_OUTPUT_MAX;
         break;
      }
      memcpy(buf + total, line, len);
      total += len;
      lines_read++;
      if (lines_read >= max_lines)
         break;
   }
   fclose(f);
   buf[total] = '\0';
   return buf;
}

char *tool_write_file(const char *path, const char *content)
{
   char resolved[MAX_PATH_LEN];
   const char *err = validate_file_path(path, resolved, sizeof(resolved));
   if (err)
      return safe_strdup(err);

   FILE *f = fopen(path, "w");
   if (!f)
   {
      char err[512];
      snprintf(err, sizeof(err), "error: cannot write %s", path);
      return safe_strdup(err);
   }
   if (content)
   {
      size_t len = strlen(content);
      if (fwrite(content, 1, len, f) != len)
      {
         fclose(f);
         return safe_strdup("error: write failed (partial write)");
      }
   }
   if (fclose(f) != 0)
      return safe_strdup("error: write failed (fclose)");
   return safe_strdup("ok");
}

char *tool_list_files(const char *path, const char *pattern)
{
   if (pattern && strstr(pattern, ".."))
      return safe_strdup("error: pattern must not contain '..'");

   char resolved[MAX_PATH_LEN];
   const char *err = guardrails_validate_file_path(path, resolved, sizeof(resolved));
   if (err)
      return safe_strdup(err);

   char glob_pat[MAX_PATH_LEN];
   if (pattern && pattern[0])
      snprintf(glob_pat, sizeof(glob_pat), "%s/%s", path, pattern);
   else
      snprintf(glob_pat, sizeof(glob_pat), "%s/*", path);

   glob_t g;
   memset(&g, 0, sizeof(g));
   int rc = glob(glob_pat, GLOB_NOSORT, NULL, &g);
   if (rc != 0 && rc != GLOB_NOMATCH)
   {
      globfree(&g);
      return safe_strdup("error: glob failed");
   }

   size_t buf_size = AGENT_TOOL_OUTPUT_MAX;
   char *buf = malloc(buf_size + 1);
   if (!buf)
   {
      globfree(&g);
      return safe_strdup("error: out of memory");
   }
   size_t pos = 0;
   int count = 0;

   for (size_t i = 0; i < g.gl_pathc && count < AGENT_MAX_LIST_FILES; i++)
   {
      size_t plen = strlen(g.gl_pathv[i]);
      if (pos + plen + 1 >= buf_size)
         break;
      memcpy(buf + pos, g.gl_pathv[i], plen);
      pos += plen;
      buf[pos++] = '\n';
      count++;
   }
   buf[pos] = '\0';

   globfree(&g);
   return buf;
}

/* Item 5: Verify tool - check assertions */

/* Direct HTTP HEAD status check via sockets + OpenSSL */
static int http_head_status(const char *url)
{
   int use_ssl;
   int port;
   const char *p;

   if (strncmp(url, "https://", 8) == 0)
   {
      use_ssl = 1;
      port = 443;
      p = url + 8;
   }
   else if (strncmp(url, "http://", 7) == 0)
   {
      use_ssl = 0;
      port = 80;
      p = url + 7;
   }
   else
      return -1;

   /* Parse host and path */
   char host[256];
   char path[2048];
   const char *slash = strchr(p, '/');
   const char *colon = strchr(p, ':');
   size_t hostlen;

   if (colon && (!slash || colon < slash))
   {
      hostlen = (size_t)(colon - p);
      port = atoi(colon + 1);
   }
   else
      hostlen = slash ? (size_t)(slash - p) : strlen(p);

   if (hostlen == 0 || hostlen >= sizeof(host))
      return -1;
   memcpy(host, p, hostlen);
   host[hostlen] = '\0';
   snprintf(path, sizeof(path), "%s", slash ? slash : "/");

   /* Connect with 5s timeout */
   char port_str[16];
   snprintf(port_str, sizeof(port_str), "%d", port);

   struct addrinfo hints = {0}, *res = NULL;
   hints.ai_family = AF_UNSPEC;
   hints.ai_socktype = SOCK_STREAM;
   if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res)
      return -1;

   int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
   if (fd < 0)
   {
      freeaddrinfo(res);
      return -1;
   }

   int flags = fcntl(fd, F_GETFL, 0);
   fcntl(fd, F_SETFL, flags | O_NONBLOCK);

   int rc = connect(fd, res->ai_addr, res->ai_addrlen);
   freeaddrinfo(res);

   if (rc < 0 && errno != EINPROGRESS)
   {
      close(fd);
      return -1;
   }
   if (rc < 0)
   {
      struct pollfd pfd = {fd, POLLOUT, 0};
      if (poll(&pfd, 1, 5000) <= 0)
      {
         close(fd);
         return -1;
      }
      int err = 0;
      socklen_t errlen = sizeof(err);
      getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
      if (err)
      {
         close(fd);
         return -1;
      }
   }
   fcntl(fd, F_SETFL, flags);

   /* Set 10s overall timeout */
   struct timeval tv = {10, 0};
   setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
   setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

   SSL *ssl = NULL;
   if (use_ssl)
   {
      /* Re-use the global SSL_CTX from agent_http_init() via a local context */
      SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
      if (!ctx)
      {
         close(fd);
         return -1;
      }
      SSL_CTX_set_default_verify_paths(ctx);
      SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);

      ssl = SSL_new(ctx);
      SSL_CTX_free(ctx); /* SSL holds a ref */
      if (!ssl)
      {
         close(fd);
         return -1;
      }
      SSL_set_fd(ssl, fd);
      SSL_set_tlsext_host_name(ssl, host);
      SSL_set1_host(ssl, host);
      if (SSL_connect(ssl) <= 0)
      {
         SSL_free(ssl);
         close(fd);
         return -1;
      }
   }

   /* Send HEAD request */
   char req[4096];
   int reqlen = snprintf(req, sizeof(req),
                         "HEAD %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);

   if (ssl)
   {
      if (SSL_write(ssl, req, reqlen) <= 0)
      {
         SSL_shutdown(ssl);
         SSL_free(ssl);
         close(fd);
         return -1;
      }
   }
   else
   {
      if (send(fd, req, (size_t)reqlen, 0) <= 0)
      {
         close(fd);
         return -1;
      }
   }

   /* Read response status line */
   char resp[4096];
   int rlen = 0;
   while (rlen < (int)sizeof(resp) - 1)
   {
      int n;
      if (ssl)
         n = SSL_read(ssl, resp + rlen, (int)sizeof(resp) - 1 - rlen);
      else
         n = (int)recv(fd, resp + rlen, sizeof(resp) - 1 - (size_t)rlen, 0);
      if (n <= 0)
         break;
      rlen += n;
      resp[rlen] = '\0';
      if (strstr(resp, "\r\n"))
         break; /* got status line at minimum */
   }

   if (ssl)
   {
      SSL_shutdown(ssl);
      SSL_free(ssl);
   }
   close(fd);

   /* Parse "HTTP/1.x NNN" */
   if (rlen < 12 || (strncmp(resp, "HTTP/1.0", 8) != 0 && strncmp(resp, "HTTP/1.1", 8) != 0))
      return -1;

   int code = atoi(resp + 9);
   return (code >= 100 && code <= 999) ? code : -1;
}

char *tool_verify(const char *check_type, const char *target, const char *expected)
{
   cJSON *result = cJSON_CreateObject();

   if (strcmp(check_type, "http_status") == 0)
   {
      /* Direct HTTP HEAD request (no shell) */
      int code = http_head_status(target);
      if (code < 0)
      {
         cJSON_AddBoolToObject(result, "pass", 0);
         cJSON_AddStringToObject(result, "reason", "HTTP request failed");
      }
      else
      {
         char status[16];
         snprintf(status, sizeof(status), "%d", code);
         int pass = expected ? (strcmp(status, expected) == 0) : (status[0] == '2');
         cJSON_AddBoolToObject(result, "pass", pass);
         cJSON_AddStringToObject(result, "actual", status);
         cJSON_AddStringToObject(result, "expected", expected ? expected : "2xx");
      }
   }
   else if (strcmp(check_type, "file_contains") == 0)
   {
      char resolved[MAX_PATH_LEN];
      const char *verr = guardrails_validate_file_path(target, resolved, sizeof(resolved));
      if (verr)
      {
         cJSON_AddBoolToObject(result, "pass", 0);
         cJSON_AddStringToObject(result, "reason", verr);
         char *json = cJSON_PrintUnformatted(result);
         cJSON_Delete(result);
         return json;
      }
      FILE *f = fopen(target, "r");
      if (!f)
      {
         cJSON_AddBoolToObject(result, "pass", 0);
         cJSON_AddStringToObject(result, "reason", "file not found");
      }
      else
      {
         char buf[AGENT_TOOL_OUTPUT_MAX + 1];
         size_t n = fread(buf, 1, AGENT_TOOL_OUTPUT_MAX, f);
         buf[n] = '\0';
         fclose(f);
         int pass = expected && strstr(buf, expected) != NULL;
         cJSON_AddBoolToObject(result, "pass", pass);
         if (!pass)
            cJSON_AddStringToObject(result, "reason", "string not found in file");
      }
   }
   else if (strcmp(check_type, "command_succeeds") == 0)
   {
      /* Reject commands with shell metacharacters */
      if (has_shell_metachar(target))
      {
         cJSON_AddBoolToObject(result, "pass", 0);
         cJSON_AddStringToObject(result, "reason", "command contains shell metacharacters");
      }
      else
      {
         /* Parse into argv and exec directly without shell */
         char *tokens[64];
         int tc = shlex_split(target, tokens, 64);
         if (tc <= 0)
         {
            cJSON_AddBoolToObject(result, "pass", 0);
            cJSON_AddStringToObject(result, "reason", "empty command");
         }
         else
         {
            const char *argv[65];
            for (int j = 0; j < tc && j < 64; j++)
               argv[j] = tokens[j];
            argv[tc] = NULL;
            char *output = NULL;
            int rc = safe_exec_capture(argv, &output, AGENT_TOOL_OUTPUT_MAX);
            int pass = (rc == 0);
            cJSON_AddBoolToObject(result, "pass", pass);
            cJSON_AddNumberToObject(result, "exit_code", rc);
            free(output);
            for (int j = 0; j < tc; j++)
               free(tokens[j]);
         }
      }
   }
   else
   {
      cJSON_AddBoolToObject(result, "pass", 0);
      cJSON_AddStringToObject(result, "reason", "unknown check_type");
   }

   char *json = cJSON_PrintUnformatted(result);
   cJSON_Delete(result);
   return json;
}

/* --- grep/search: pattern search in files with regex support --- */

char *tool_grep(const char *path, const char *pattern, int max_results)
{
   if (!path || !pattern)
      return safe_strdup("error: missing path or pattern");
   if (max_results <= 0)
      max_results = 50;
   if (max_results > 200)
      max_results = 200;

   char resolved[MAX_PATH_LEN];
   const char *verr = guardrails_validate_file_path(path, resolved, sizeof(resolved));
   if (verr)
      return safe_strdup(verr);

   struct stat st;
   if (stat(path, &st) != 0)
      return safe_strdup("error: path does not exist");

   char max_str[16];
   snprintf(max_str, sizeof(max_str), "%d", max_results);

   const char *argv[] = {"grep", "-rn", "--include=*", "-m", max_str, "--", pattern, path, NULL};
   char *output = NULL;
   int rc = safe_exec_capture(argv, &output, AGENT_TOOL_OUTPUT_MAX);

   if (rc != 0 && rc != 1 && (!output || !output[0]))
   {
      free(output);
      return safe_strdup("no matches found");
   }

   return output ? output : safe_strdup("no matches found");
}

/* --- git_diff: show working tree changes --- */

char *tool_git_diff(const char *repo_path, const char *ref)
{
   if (!repo_path)
      return safe_strdup("error: missing repo path");

   struct stat st;
   if (stat(repo_path, &st) != 0 || !S_ISDIR(st.st_mode))
      return safe_strdup("error: repo path is not a directory");

   const char *argv[8];
   int ai = 0;
   argv[ai++] = "git";
   argv[ai++] = "-C";
   argv[ai++] = repo_path;
   argv[ai++] = "diff";
   if (ref && ref[0])
      argv[ai++] = ref;
   argv[ai] = NULL;

   char *output = NULL;
   int rc = safe_exec_capture(argv, &output, AGENT_TOOL_OUTPUT_MAX);
   if (rc != 0 && (!output || !output[0]))
   {
      free(output);
      return safe_strdup("error: git diff failed");
   }
   return output ? output : safe_strdup("");
}

/* --- git_status: show working tree status --- */

char *tool_git_status(const char *repo_path)
{
   if (!repo_path)
      return safe_strdup("error: missing repo path");

   struct stat st;
   if (stat(repo_path, &st) != 0 || !S_ISDIR(st.st_mode))
      return safe_strdup("error: repo path is not a directory");

   const char *argv[] = {"git", "-C", repo_path, "status", "--porcelain", NULL};
   char *output = NULL;
   int rc = safe_exec_capture(argv, &output, AGENT_TOOL_OUTPUT_MAX);
   if (rc != 0 && (!output || !output[0]))
   {
      free(output);
      return safe_strdup("error: git status failed");
   }
   return output ? output : safe_strdup("");
}

/* --- env_get: query environment variables safely --- */

char *tool_env_get(const char *name)
{
   if (!name || !name[0])
      return safe_strdup("error: missing variable name");

   /* Reject names with shell metacharacters */
   for (const char *p = name; *p; p++)
   {
      if (!isalnum((unsigned char)*p) && *p != '_')
         return safe_strdup("error: invalid variable name");
   }

   const char *val = getenv(name);
   if (!val)
      return safe_strdup("(not set)");
   return safe_strdup(val);
}

/* --- test: check file/dir existence, permissions, types --- */

char *tool_test(const char *path, const char *check)
{
   if (!path)
      return safe_strdup("{\"pass\":false,\"reason\":\"missing path\"}");
   if (!check)
      check = "exists";

   struct stat st;
   int exists = (lstat(path, &st) == 0);

   cJSON *result = cJSON_CreateObject();

   if (strcmp(check, "exists") == 0)
   {
      cJSON_AddBoolToObject(result, "pass", exists);
      if (exists)
      {
         cJSON_AddStringToObject(result, "type",
                                 S_ISDIR(st.st_mode)   ? "directory"
                                 : S_ISLNK(st.st_mode) ? "symlink"
                                 : S_ISREG(st.st_mode) ? "file"
                                                       : "other");
         cJSON_AddNumberToObject(result, "size", (double)st.st_size);
      }
   }
   else if (strcmp(check, "is_file") == 0)
   {
      cJSON_AddBoolToObject(result, "pass", exists && S_ISREG(st.st_mode));
   }
   else if (strcmp(check, "is_dir") == 0)
   {
      cJSON_AddBoolToObject(result, "pass", exists && S_ISDIR(st.st_mode));
   }
   else if (strcmp(check, "readable") == 0)
   {
      cJSON_AddBoolToObject(result, "pass", access(path, R_OK) == 0);
   }
   else if (strcmp(check, "writable") == 0)
   {
      cJSON_AddBoolToObject(result, "pass", access(path, W_OK) == 0);
   }
   else if (strcmp(check, "executable") == 0)
   {
      cJSON_AddBoolToObject(result, "pass", access(path, X_OK) == 0);
   }
   else
   {
      cJSON_AddBoolToObject(result, "pass", 0);
      cJSON_AddStringToObject(result, "reason", "unknown check type");
   }

   char *json = cJSON_PrintUnformatted(result);
   cJSON_Delete(result);
   return json;
}

/* Item 7: Git log tool (safe: no shell, uses fork/exec) */
char *tool_git_log(const char *repo_path, int count)
{
   if (count <= 0)
      count = 10;
   if (count > 50)
      count = 50;

   /* Validate repo_path is a directory */
   struct stat st;
   if (stat(repo_path, &st) != 0 || !S_ISDIR(st.st_mode))
      return safe_strdup("error: repo path is not a directory");

   char count_str[16];
   snprintf(count_str, sizeof(count_str), "%d", count);

   const char *argv[] = {"git", "-C", repo_path, "log", "--oneline", "-n", count_str, NULL};
   char *output = NULL;
   int rc = safe_exec_capture(argv, &output, AGENT_TOOL_OUTPUT_MAX);

   if (rc != 0 && (!output || !output[0]))
   {
      free(output);
      return safe_strdup("error: git log failed");
   }

   return output ? output : safe_strdup("");
}

/* Map internal tool arg format to guardrail-compatible JSON.
 * Guardrails expect "file_path" for edit tools and "command" for Bash. */
static char *guardrail_input_json(const char *name, const char *arguments_json)
{
   cJSON *args = cJSON_Parse(arguments_json);
   if (!args)
      return safe_strdup(arguments_json);

   cJSON *mapped = cJSON_CreateObject();
   if (strcmp(name, "bash") == 0)
   {
      cJSON *cmd = cJSON_GetObjectItem(args, "command");
      if (cmd && cJSON_IsString(cmd))
         cJSON_AddStringToObject(mapped, "command", cmd->valuestring);
   }
   else if (strcmp(name, "write_file") == 0)
   {
      cJSON *p = cJSON_GetObjectItem(args, "path");
      if (p && cJSON_IsString(p))
         cJSON_AddStringToObject(mapped, "file_path", p->valuestring);
   }
   else if (strcmp(name, "read_file") == 0)
   {
      cJSON *p = cJSON_GetObjectItem(args, "path");
      if (p && cJSON_IsString(p))
         cJSON_AddStringToObject(mapped, "file_path", p->valuestring);
   }
   else
   {
      /* Pass through original */
      cJSON_Delete(mapped);
      cJSON_Delete(args);
      return safe_strdup(arguments_json);
   }

   char *json = cJSON_PrintUnformatted(mapped);
   cJSON_Delete(mapped);
   cJSON_Delete(args);
   return json ? json : safe_strdup(arguments_json);
}

/* Delegation conversation: request input from parent agent.
 * delegation_request_input is provided by server_compute.c when running as delegate.
 * Default stub returns NULL; the server overrides this at link time. */
char *delegation_request_input(const char *question) __attribute__((weak));
char *delegation_request_input(const char *question)
{
   (void)question;
   return NULL;
}

char *tool_request_input(const char *question)
{
   if (!question || !question[0])
      return safe_strdup("error: missing question");

   char *reply = delegation_request_input(question);
   if (!reply)
      return safe_strdup("error: request_input is only available during delegated execution");

   return reply;
}

char *tool_code_search(const char *query, const char *project, int max_results)
{
   if (!query || !query[0])
      return safe_strdup("error: missing query");

   if (max_results <= 0)
      max_results = 50;
   if (max_results > 200)
      max_results = 200;

   config_t cfg;
   config_load(&cfg);
   sqlite3 *db = db_open_fast(cfg.db_path);
   if (!db)
      return safe_strdup("error: cannot open database");

   code_search_hit_t *hits = calloc((size_t)max_results, sizeof(code_search_hit_t));
   if (!hits)
   {
      sqlite3_close(db);
      return safe_strdup("error: out of memory");
   }

   int count = index_code_search(db, query, project, hits, max_results);
   sqlite3_close(db);

   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; i < count; i++)
   {
      cJSON *h = cJSON_CreateObject();
      cJSON_AddStringToObject(h, "project", hits[i].project);
      cJSON_AddStringToObject(h, "file", hits[i].file_path);
      cJSON_AddStringToObject(h, "snippet", hits[i].snippet);
      cJSON_AddNumberToObject(h, "rank", hits[i].rank);
      cJSON_AddItemToArray(arr, h);
   }
   free(hits);

   char *json = cJSON_PrintUnformatted(arr);
   cJSON_Delete(arr);
   return json ? json : safe_strdup("[]");
}

char *dispatch_tool_call_ctx(const char *name, const char *arguments_json, int timeout_ms,
                             checkpoint_ctx_t *ctx)
{
   cJSON *args = cJSON_Parse(arguments_json);
   if (!args)
      return safe_strdup("error: invalid arguments JSON");

   /* --- Guardrail enforcement for ALL tool execution paths --- */
   {
      config_t cfg;
      config_load(&cfg);
      sqlite3 *guard_db = db_open_fast(cfg.db_path);

      char state_path[MAX_PATH_LEN];
      session_state_path(state_path, sizeof(state_path));
      session_state_t state;
      session_state_load(&state, state_path);

      char cwd[MAX_PATH_LEN];
      if (!getcwd(cwd, sizeof(cwd)))
         cwd[0] = '\0';

      const char *gr_name = guardrails_canonical_tool_name(name);
      char *gr_input = guardrail_input_json(name, arguments_json);

      char msg[1024] = "";
      int rc = pre_tool_check(guard_db, gr_name, gr_input, &state, config_guardrail_mode(&cfg), cwd,
                              msg, sizeof(msg));

      session_state_save(&state, state_path);
      free(gr_input);

      if (guard_db)
         sqlite3_close(guard_db);

      if (rc == 1 && msg[0])
      {
         /* Worktree path rewrite: update file_path in args and continue */
         cJSON *fp_arg = cJSON_GetObjectItem(args, "file_path");
         if (!fp_arg)
            fp_arg = cJSON_GetObjectItem(args, "path");
         if (fp_arg)
            cJSON_SetValuestring(fp_arg, msg);
      }
      else if (rc != 0)
      {
         /* Tool blocked by guardrails */
         cJSON_Delete(args);
         char err[1200];
         snprintf(err, sizeof(err), "error: guardrail blocked: %s", msg);
         return safe_strdup(err);
      }
   }

   char *result = NULL;

   if (strcmp(name, "bash") == 0)
   {
      cJSON *cmd = cJSON_GetObjectItem(args, "command");
      if (cmd && cJSON_IsString(cmd))
         result = tool_bash(cmd->valuestring, timeout_ms);
      else
         result = safe_strdup("error: missing 'command' parameter");
   }
   else if (strcmp(name, "read_file") == 0)
   {
      cJSON *p = cJSON_GetObjectItem(args, "path");
      if (!p || !cJSON_IsString(p))
      {
         result = safe_strdup("error: missing 'path' parameter");
      }
      else
      {
         cJSON *off = cJSON_GetObjectItem(args, "offset");
         cJSON *lim = cJSON_GetObjectItem(args, "limit");
         int offset = (off && cJSON_IsNumber(off)) ? off->valueint : 0;
         int limit = (lim && cJSON_IsNumber(lim)) ? lim->valueint : 0;
         result = tool_read_file(p->valuestring, offset, limit);
      }
   }
   else if (strcmp(name, "write_file") == 0)
   {
      cJSON *p = cJSON_GetObjectItem(args, "path");
      cJSON *c = cJSON_GetObjectItem(args, "content");
      if (!p || !cJSON_IsString(p))
         result = safe_strdup("error: missing 'path' parameter");
      else
      {
         /* Capture checkpoint before write */
         checkpoint_ctx_push(ctx, p->valuestring);
         result = tool_write_file(p->valuestring, c && cJSON_IsString(c) ? c->valuestring : "");
      }
   }
   else if (strcmp(name, "list_files") == 0)
   {
      cJSON *p = cJSON_GetObjectItem(args, "path");
      if (!p || !cJSON_IsString(p))
      {
         result = safe_strdup("error: missing 'path' parameter");
      }
      else
      {
         cJSON *pat = cJSON_GetObjectItem(args, "pattern");
         result = tool_list_files(p->valuestring,
                                  (pat && cJSON_IsString(pat)) ? pat->valuestring : NULL);
      }
   }
   else if (strcmp(name, "verify") == 0)
   {
      cJSON *ct = cJSON_GetObjectItem(args, "check_type");
      cJSON *tgt = cJSON_GetObjectItem(args, "target");
      cJSON *exp = cJSON_GetObjectItem(args, "expected");
      if (!ct || !cJSON_IsString(ct) || !tgt || !cJSON_IsString(tgt))
         result = safe_strdup("error: missing 'check_type' or 'target'");
      else
         result = tool_verify(ct->valuestring, tgt->valuestring,
                              (exp && cJSON_IsString(exp)) ? exp->valuestring : NULL);
   }
   else if (strcmp(name, "git_log") == 0)
   {
      cJSON *p = cJSON_GetObjectItem(args, "path");
      cJSON *n = cJSON_GetObjectItem(args, "count");
      if (!p || !cJSON_IsString(p))
         result = safe_strdup("error: missing 'path' parameter");
      else
         result = tool_git_log(p->valuestring, (n && cJSON_IsNumber(n)) ? n->valueint : 10);
   }
   else if (strcmp(name, "grep") == 0)
   {
      cJSON *p = cJSON_GetObjectItem(args, "path");
      cJSON *pat = cJSON_GetObjectItem(args, "pattern");
      cJSON *mx = cJSON_GetObjectItem(args, "max_results");
      if (!p || !cJSON_IsString(p) || !pat || !cJSON_IsString(pat))
         result = safe_strdup("error: missing 'path' or 'pattern' parameter");
      else
         result = tool_grep(p->valuestring, pat->valuestring,
                            (mx && cJSON_IsNumber(mx)) ? mx->valueint : 50);
   }
   else if (strcmp(name, "git_diff") == 0)
   {
      cJSON *p = cJSON_GetObjectItem(args, "path");
      cJSON *r = cJSON_GetObjectItem(args, "ref");
      if (!p || !cJSON_IsString(p))
         result = safe_strdup("error: missing 'path' parameter");
      else
         result = tool_git_diff(p->valuestring, (r && cJSON_IsString(r)) ? r->valuestring : NULL);
   }
   else if (strcmp(name, "git_status") == 0)
   {
      cJSON *p = cJSON_GetObjectItem(args, "path");
      if (!p || !cJSON_IsString(p))
         result = safe_strdup("error: missing 'path' parameter");
      else
         result = tool_git_status(p->valuestring);
   }
   else if (strcmp(name, "env_get") == 0)
   {
      cJSON *n = cJSON_GetObjectItem(args, "name");
      if (!n || !cJSON_IsString(n))
         result = safe_strdup("error: missing 'name' parameter");
      else
         result = tool_env_get(n->valuestring);
   }
   else if (strcmp(name, "test") == 0)
   {
      cJSON *p = cJSON_GetObjectItem(args, "path");
      cJSON *c = cJSON_GetObjectItem(args, "check");
      if (!p || !cJSON_IsString(p))
         result = safe_strdup("error: missing 'path' parameter");
      else
         result = tool_test(p->valuestring, (c && cJSON_IsString(c)) ? c->valuestring : NULL);
   }
   else if (strcmp(name, "request_input") == 0)
   {
      cJSON *q = cJSON_GetObjectItem(args, "question");
      if (!q || !cJSON_IsString(q))
         result = safe_strdup("error: missing 'question' parameter");
      else
         result = tool_request_input(q->valuestring);
   }
   else if (strcmp(name, "code_search") == 0)
   {
      cJSON *q = cJSON_GetObjectItem(args, "query");
      cJSON *p = cJSON_GetObjectItem(args, "project");
      cJSON *mx = cJSON_GetObjectItem(args, "max_results");
      if (!q || !cJSON_IsString(q))
         result = safe_strdup("error: missing 'query' parameter");
      else
         result = tool_code_search(q->valuestring, (p && cJSON_IsString(p)) ? p->valuestring : NULL,
                                   (mx && cJSON_IsNumber(mx)) ? mx->valueint : 50);
   }
   else
   {
      char err[128];
      snprintf(err, sizeof(err), "error: unknown tool '%s'", name);
      result = safe_strdup(err);
   }

   cJSON_Delete(args);
   return result;
}

/* Legacy dispatch: uses the process-global compat checkpoint context */
char *dispatch_tool_call(const char *name, const char *arguments_json, int timeout_ms)
{
   return dispatch_tool_call_ctx(name, arguments_json, timeout_ms, &g_compat_ctx);
}

#endif /* !_WIN32 */

/* --- Tool definitions JSON --- */

cJSON *build_tools_array(void)
{
   cJSON *tools = cJSON_CreateArray();

   /* bash */
   {
      cJSON *tool = cJSON_CreateObject();
      cJSON_AddStringToObject(tool, "type", "function");
      cJSON *fn = cJSON_CreateObject();
      cJSON_AddStringToObject(fn, "name", "bash");
      cJSON_AddStringToObject(fn, "description",
                              "Run a shell command. Returns JSON with stdout, stderr, exit_code.");
      cJSON *params = cJSON_CreateObject();
      cJSON_AddStringToObject(params, "type", "object");
      cJSON *props = cJSON_CreateObject();
      cJSON *cmd_prop = cJSON_CreateObject();
      cJSON_AddStringToObject(cmd_prop, "type", "string");
      cJSON_AddStringToObject(cmd_prop, "description", "The shell command to execute");
      cJSON_AddItemToObject(props, "command", cmd_prop);
      cJSON_AddItemToObject(params, "properties", props);
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("command"));
      cJSON_AddItemToObject(params, "required", req);
      cJSON_AddItemToObject(fn, "parameters", params);
      cJSON_AddItemToObject(tool, "function", fn);
      cJSON_AddItemToArray(tools, tool);
   }

   /* read_file */
   {
      cJSON *tool = cJSON_CreateObject();
      cJSON_AddStringToObject(tool, "type", "function");
      cJSON *fn = cJSON_CreateObject();
      cJSON_AddStringToObject(fn, "name", "read_file");
      cJSON_AddStringToObject(fn, "description", "Read a file and return its contents.");
      cJSON *params = cJSON_CreateObject();
      cJSON_AddStringToObject(params, "type", "object");
      cJSON *props = cJSON_CreateObject();
      cJSON *p_path = cJSON_CreateObject();
      cJSON_AddStringToObject(p_path, "type", "string");
      cJSON_AddStringToObject(p_path, "description", "Absolute path to the file");
      cJSON_AddItemToObject(props, "path", p_path);
      cJSON *p_off = cJSON_CreateObject();
      cJSON_AddStringToObject(p_off, "type", "integer");
      cJSON_AddStringToObject(p_off, "description", "Line offset to start reading from");
      cJSON_AddItemToObject(props, "offset", p_off);
      cJSON *p_lim = cJSON_CreateObject();
      cJSON_AddStringToObject(p_lim, "type", "integer");
      cJSON_AddStringToObject(p_lim, "description", "Maximum number of lines to read");
      cJSON_AddItemToObject(props, "limit", p_lim);
      cJSON_AddItemToObject(params, "properties", props);
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("path"));
      cJSON_AddItemToObject(params, "required", req);
      cJSON_AddItemToObject(fn, "parameters", params);
      cJSON_AddItemToObject(tool, "function", fn);
      cJSON_AddItemToArray(tools, tool);
   }

   /* write_file */
   {
      cJSON *tool = cJSON_CreateObject();
      cJSON_AddStringToObject(tool, "type", "function");
      cJSON *fn = cJSON_CreateObject();
      cJSON_AddStringToObject(fn, "name", "write_file");
      cJSON_AddStringToObject(fn, "description", "Write content to a file (overwrites).");
      cJSON *params = cJSON_CreateObject();
      cJSON_AddStringToObject(params, "type", "object");
      cJSON *props = cJSON_CreateObject();
      cJSON *p_path = cJSON_CreateObject();
      cJSON_AddStringToObject(p_path, "type", "string");
      cJSON_AddStringToObject(p_path, "description", "Absolute path to write to");
      cJSON_AddItemToObject(props, "path", p_path);
      cJSON *p_content = cJSON_CreateObject();
      cJSON_AddStringToObject(p_content, "type", "string");
      cJSON_AddStringToObject(p_content, "description", "Content to write");
      cJSON_AddItemToObject(props, "content", p_content);
      cJSON_AddItemToObject(params, "properties", props);
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("path"));
      cJSON_AddItemToArray(req, cJSON_CreateString("content"));
      cJSON_AddItemToObject(params, "required", req);
      cJSON_AddItemToObject(fn, "parameters", params);
      cJSON_AddItemToObject(tool, "function", fn);
      cJSON_AddItemToArray(tools, tool);
   }

   /* list_files */
   {
      cJSON *tool = cJSON_CreateObject();
      cJSON_AddStringToObject(tool, "type", "function");
      cJSON *fn = cJSON_CreateObject();
      cJSON_AddStringToObject(fn, "name", "list_files");
      cJSON_AddStringToObject(fn, "description",
                              "List files in a directory, optionally matching a glob pattern.");
      cJSON *params = cJSON_CreateObject();
      cJSON_AddStringToObject(params, "type", "object");
      cJSON *props = cJSON_CreateObject();
      cJSON *p_path = cJSON_CreateObject();
      cJSON_AddStringToObject(p_path, "type", "string");
      cJSON_AddStringToObject(p_path, "description", "Directory path to list");
      cJSON_AddItemToObject(props, "path", p_path);
      cJSON *p_pat = cJSON_CreateObject();
      cJSON_AddStringToObject(p_pat, "type", "string");
      cJSON_AddStringToObject(p_pat, "description", "Glob pattern to filter files");
      cJSON_AddItemToObject(props, "pattern", p_pat);
      cJSON_AddItemToObject(params, "properties", props);
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("path"));
      cJSON_AddItemToObject(params, "required", req);
      cJSON_AddItemToObject(fn, "parameters", params);
      cJSON_AddItemToObject(tool, "function", fn);
      cJSON_AddItemToArray(tools, tool);
   }

   /* verify */
   {
      cJSON *tool = cJSON_CreateObject();
      cJSON_AddStringToObject(tool, "type", "function");
      cJSON *fn = cJSON_CreateObject();
      cJSON_AddStringToObject(fn, "name", "verify");
      cJSON_AddStringToObject(
          fn, "description",
          "Verify an assertion. check_type: http_status, file_contains, command_succeeds.");
      cJSON *params = cJSON_CreateObject();
      cJSON_AddStringToObject(params, "type", "object");
      cJSON *props = cJSON_CreateObject();
      cJSON *p1 = cJSON_CreateObject();
      cJSON_AddStringToObject(p1, "type", "string");
      cJSON_AddStringToObject(p1, "description", "http_status, file_contains, or command_succeeds");
      cJSON_AddItemToObject(props, "check_type", p1);
      cJSON *p2 = cJSON_CreateObject();
      cJSON_AddStringToObject(p2, "type", "string");
      cJSON_AddStringToObject(p2, "description", "URL, file path, or command to check");
      cJSON_AddItemToObject(props, "target", p2);
      cJSON *p3 = cJSON_CreateObject();
      cJSON_AddStringToObject(p3, "type", "string");
      cJSON_AddStringToObject(p3, "description", "Expected value (optional)");
      cJSON_AddItemToObject(props, "expected", p3);
      cJSON_AddItemToObject(params, "properties", props);
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("check_type"));
      cJSON_AddItemToArray(req, cJSON_CreateString("target"));
      cJSON_AddItemToObject(params, "required", req);
      cJSON_AddItemToObject(fn, "parameters", params);
      cJSON_AddItemToObject(tool, "function", fn);
      cJSON_AddItemToArray(tools, tool);
   }

   /* git_log */
   {
      cJSON *tool = cJSON_CreateObject();
      cJSON_AddStringToObject(tool, "type", "function");
      cJSON *fn = cJSON_CreateObject();
      cJSON_AddStringToObject(fn, "name", "git_log");
      cJSON_AddStringToObject(fn, "description", "Show recent git commits for a repository.");
      cJSON *params = cJSON_CreateObject();
      cJSON_AddStringToObject(params, "type", "object");
      cJSON *props = cJSON_CreateObject();
      cJSON *p1 = cJSON_CreateObject();
      cJSON_AddStringToObject(p1, "type", "string");
      cJSON_AddStringToObject(p1, "description", "Path to the git repository");
      cJSON_AddItemToObject(props, "path", p1);
      cJSON *p2 = cJSON_CreateObject();
      cJSON_AddStringToObject(p2, "type", "integer");
      cJSON_AddStringToObject(p2, "description", "Number of commits (default 10)");
      cJSON_AddItemToObject(props, "count", p2);
      cJSON_AddItemToObject(params, "properties", props);
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("path"));
      cJSON_AddItemToObject(params, "required", req);
      cJSON_AddItemToObject(fn, "parameters", params);
      cJSON_AddItemToObject(tool, "function", fn);
      cJSON_AddItemToArray(tools, tool);
   }

   /* grep */
   {
      cJSON *tool = cJSON_CreateObject();
      cJSON_AddStringToObject(tool, "type", "function");
      cJSON *fn = cJSON_CreateObject();
      cJSON_AddStringToObject(fn, "name", "grep");
      cJSON_AddStringToObject(
          fn, "description",
          "Search for a pattern in files. Returns matching lines with file:line.");
      cJSON *params = cJSON_CreateObject();
      cJSON_AddStringToObject(params, "type", "object");
      cJSON *props = cJSON_CreateObject();
      cJSON *p1 = cJSON_CreateObject();
      cJSON_AddStringToObject(p1, "type", "string");
      cJSON_AddStringToObject(p1, "description", "Directory or file to search in");
      cJSON_AddItemToObject(props, "path", p1);
      cJSON *p2 = cJSON_CreateObject();
      cJSON_AddStringToObject(p2, "type", "string");
      cJSON_AddStringToObject(p2, "description", "Pattern to search for (basic regex)");
      cJSON_AddItemToObject(props, "pattern", p2);
      cJSON *p3 = cJSON_CreateObject();
      cJSON_AddStringToObject(p3, "type", "integer");
      cJSON_AddStringToObject(p3, "description", "Max results (default 50)");
      cJSON_AddItemToObject(props, "max_results", p3);
      cJSON_AddItemToObject(params, "properties", props);
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("path"));
      cJSON_AddItemToArray(req, cJSON_CreateString("pattern"));
      cJSON_AddItemToObject(params, "required", req);
      cJSON_AddItemToObject(fn, "parameters", params);
      cJSON_AddItemToObject(tool, "function", fn);
      cJSON_AddItemToArray(tools, tool);
   }

   /* git_diff */
   {
      cJSON *tool = cJSON_CreateObject();
      cJSON_AddStringToObject(tool, "type", "function");
      cJSON *fn = cJSON_CreateObject();
      cJSON_AddStringToObject(fn, "name", "git_diff");
      cJSON_AddStringToObject(fn, "description",
                              "Show git diff for a repository. Optionally diff against a ref.");
      cJSON *params = cJSON_CreateObject();
      cJSON_AddStringToObject(params, "type", "object");
      cJSON *props = cJSON_CreateObject();
      cJSON *p1 = cJSON_CreateObject();
      cJSON_AddStringToObject(p1, "type", "string");
      cJSON_AddStringToObject(p1, "description", "Path to the git repository");
      cJSON_AddItemToObject(props, "path", p1);
      cJSON *p2 = cJSON_CreateObject();
      cJSON_AddStringToObject(p2, "type", "string");
      cJSON_AddStringToObject(p2, "description", "Git ref to diff against (optional)");
      cJSON_AddItemToObject(props, "ref", p2);
      cJSON_AddItemToObject(params, "properties", props);
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("path"));
      cJSON_AddItemToObject(params, "required", req);
      cJSON_AddItemToObject(fn, "parameters", params);
      cJSON_AddItemToObject(tool, "function", fn);
      cJSON_AddItemToArray(tools, tool);
   }

   /* git_status */
   {
      cJSON *tool = cJSON_CreateObject();
      cJSON_AddStringToObject(tool, "type", "function");
      cJSON *fn = cJSON_CreateObject();
      cJSON_AddStringToObject(fn, "name", "git_status");
      cJSON_AddStringToObject(fn, "description",
                              "Show git status (porcelain format) for a repository.");
      cJSON *params = cJSON_CreateObject();
      cJSON_AddStringToObject(params, "type", "object");
      cJSON *props = cJSON_CreateObject();
      cJSON *p1 = cJSON_CreateObject();
      cJSON_AddStringToObject(p1, "type", "string");
      cJSON_AddStringToObject(p1, "description", "Path to the git repository");
      cJSON_AddItemToObject(props, "path", p1);
      cJSON_AddItemToObject(params, "properties", props);
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("path"));
      cJSON_AddItemToObject(params, "required", req);
      cJSON_AddItemToObject(fn, "parameters", params);
      cJSON_AddItemToObject(tool, "function", fn);
      cJSON_AddItemToArray(tools, tool);
   }

   /* env_get */
   {
      cJSON *tool = cJSON_CreateObject();
      cJSON_AddStringToObject(tool, "type", "function");
      cJSON *fn = cJSON_CreateObject();
      cJSON_AddStringToObject(fn, "name", "env_get");
      cJSON_AddStringToObject(fn, "description", "Get the value of an environment variable.");
      cJSON *params = cJSON_CreateObject();
      cJSON_AddStringToObject(params, "type", "object");
      cJSON *props = cJSON_CreateObject();
      cJSON *p1 = cJSON_CreateObject();
      cJSON_AddStringToObject(p1, "type", "string");
      cJSON_AddStringToObject(p1, "description", "Environment variable name");
      cJSON_AddItemToObject(props, "name", p1);
      cJSON_AddItemToObject(params, "properties", props);
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("name"));
      cJSON_AddItemToObject(params, "required", req);
      cJSON_AddItemToObject(fn, "parameters", params);
      cJSON_AddItemToObject(tool, "function", fn);
      cJSON_AddItemToArray(tools, tool);
   }

   /* test */
   {
      cJSON *tool = cJSON_CreateObject();
      cJSON_AddStringToObject(tool, "type", "function");
      cJSON *fn = cJSON_CreateObject();
      cJSON_AddStringToObject(fn, "name", "test");
      cJSON_AddStringToObject(fn, "description",
                              "Check file/dir existence, type, and permissions. "
                              "check: exists, is_file, is_dir, readable, writable, executable.");
      cJSON *params = cJSON_CreateObject();
      cJSON_AddStringToObject(params, "type", "object");
      cJSON *props = cJSON_CreateObject();
      cJSON *p1 = cJSON_CreateObject();
      cJSON_AddStringToObject(p1, "type", "string");
      cJSON_AddStringToObject(p1, "description", "Path to check");
      cJSON_AddItemToObject(props, "path", p1);
      cJSON *p2 = cJSON_CreateObject();
      cJSON_AddStringToObject(p2, "type", "string");
      cJSON_AddStringToObject(
          p2, "description", "Check type: exists, is_file, is_dir, readable, writable, executable");
      cJSON_AddItemToObject(props, "check", p2);
      cJSON_AddItemToObject(params, "properties", props);
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("path"));
      cJSON_AddItemToObject(params, "required", req);
      cJSON_AddItemToObject(fn, "parameters", params);
      cJSON_AddItemToObject(tool, "function", fn);
      cJSON_AddItemToArray(tools, tool);
   }

   /* code_search */
   {
      cJSON *tool = cJSON_CreateObject();
      cJSON_AddStringToObject(tool, "type", "function");
      cJSON *fn = cJSON_CreateObject();
      cJSON_AddStringToObject(fn, "name", "code_search");
      cJSON_AddStringToObject(
          fn, "description",
          "Full-text search across indexed code files. Returns ranked results with snippets.");
      cJSON *params = cJSON_CreateObject();
      cJSON_AddStringToObject(params, "type", "object");
      cJSON *props = cJSON_CreateObject();
      cJSON *p1 = cJSON_CreateObject();
      cJSON_AddStringToObject(p1, "type", "string");
      cJSON_AddStringToObject(p1, "description", "Search query (FTS5 syntax)");
      cJSON_AddItemToObject(props, "query", p1);
      cJSON *p2 = cJSON_CreateObject();
      cJSON_AddStringToObject(p2, "type", "string");
      cJSON_AddStringToObject(p2, "description", "Project name to search in (optional)");
      cJSON_AddItemToObject(props, "project", p2);
      cJSON *p3 = cJSON_CreateObject();
      cJSON_AddStringToObject(p3, "type", "integer");
      cJSON_AddStringToObject(p3, "description", "Max results (default 50)");
      cJSON_AddItemToObject(props, "max_results", p3);
      cJSON_AddItemToObject(params, "properties", props);
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("query"));
      cJSON_AddItemToObject(params, "required", req);
      cJSON_AddItemToObject(fn, "parameters", params);
      cJSON_AddItemToObject(tool, "function", fn);
      cJSON_AddItemToArray(tools, tool);
   }

   return tools;
}

/* Build tools array for Responses API (flat format: type, name, description, parameters) */
cJSON *build_tools_array_responses(void)
{
   cJSON *tools = cJSON_CreateArray();

   /* bash */
   {
      cJSON *tool = cJSON_CreateObject();
      cJSON_AddStringToObject(tool, "type", "function");
      cJSON_AddStringToObject(tool, "name", "bash");
      cJSON_AddStringToObject(tool, "description",
                              "Run a shell command. Returns JSON with stdout, stderr, exit_code.");
      cJSON *params = cJSON_CreateObject();
      cJSON_AddStringToObject(params, "type", "object");
      cJSON *props = cJSON_CreateObject();
      cJSON *cmd_prop = cJSON_CreateObject();
      cJSON_AddStringToObject(cmd_prop, "type", "string");
      cJSON_AddStringToObject(cmd_prop, "description", "The shell command to execute");
      cJSON_AddItemToObject(props, "command", cmd_prop);
      cJSON_AddItemToObject(params, "properties", props);
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("command"));
      cJSON_AddItemToObject(params, "required", req);
      cJSON_AddItemToObject(tool, "parameters", params);
      cJSON_AddItemToArray(tools, tool);
   }

   /* read_file */
   {
      cJSON *tool = cJSON_CreateObject();
      cJSON_AddStringToObject(tool, "type", "function");
      cJSON_AddStringToObject(tool, "name", "read_file");
      cJSON_AddStringToObject(tool, "description", "Read a file and return its contents.");
      cJSON *params = cJSON_CreateObject();
      cJSON_AddStringToObject(params, "type", "object");
      cJSON *props = cJSON_CreateObject();
      cJSON *p_path = cJSON_CreateObject();
      cJSON_AddStringToObject(p_path, "type", "string");
      cJSON_AddStringToObject(p_path, "description", "Absolute path to the file");
      cJSON_AddItemToObject(props, "path", p_path);
      cJSON *p_off = cJSON_CreateObject();
      cJSON_AddStringToObject(p_off, "type", "integer");
      cJSON_AddStringToObject(p_off, "description", "Line offset to start reading from");
      cJSON_AddItemToObject(props, "offset", p_off);
      cJSON *p_lim = cJSON_CreateObject();
      cJSON_AddStringToObject(p_lim, "type", "integer");
      cJSON_AddStringToObject(p_lim, "description", "Maximum number of lines to read");
      cJSON_AddItemToObject(props, "limit", p_lim);
      cJSON_AddItemToObject(params, "properties", props);
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("path"));
      cJSON_AddItemToObject(params, "required", req);
      cJSON_AddItemToObject(tool, "parameters", params);
      cJSON_AddItemToArray(tools, tool);
   }

   /* write_file */
   {
      cJSON *tool = cJSON_CreateObject();
      cJSON_AddStringToObject(tool, "type", "function");
      cJSON_AddStringToObject(tool, "name", "write_file");
      cJSON_AddStringToObject(tool, "description", "Write content to a file (overwrites).");
      cJSON *params = cJSON_CreateObject();
      cJSON_AddStringToObject(params, "type", "object");
      cJSON *props = cJSON_CreateObject();
      cJSON *p_path = cJSON_CreateObject();
      cJSON_AddStringToObject(p_path, "type", "string");
      cJSON_AddStringToObject(p_path, "description", "Absolute path to write to");
      cJSON_AddItemToObject(props, "path", p_path);
      cJSON *p_content = cJSON_CreateObject();
      cJSON_AddStringToObject(p_content, "type", "string");
      cJSON_AddStringToObject(p_content, "description", "Content to write");
      cJSON_AddItemToObject(props, "content", p_content);
      cJSON_AddItemToObject(params, "properties", props);
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("path"));
      cJSON_AddItemToArray(req, cJSON_CreateString("content"));
      cJSON_AddItemToObject(params, "required", req);
      cJSON_AddItemToObject(tool, "parameters", params);
      cJSON_AddItemToArray(tools, tool);
   }

   /* list_files */
   {
      cJSON *tool = cJSON_CreateObject();
      cJSON_AddStringToObject(tool, "type", "function");
      cJSON_AddStringToObject(tool, "name", "list_files");
      cJSON_AddStringToObject(tool, "description",
                              "List files in a directory, optionally matching a glob pattern.");
      cJSON *params = cJSON_CreateObject();
      cJSON_AddStringToObject(params, "type", "object");
      cJSON *props = cJSON_CreateObject();
      cJSON *p_path = cJSON_CreateObject();
      cJSON_AddStringToObject(p_path, "type", "string");
      cJSON_AddStringToObject(p_path, "description", "Directory path to list");
      cJSON_AddItemToObject(props, "path", p_path);
      cJSON *p_pat = cJSON_CreateObject();
      cJSON_AddStringToObject(p_pat, "type", "string");
      cJSON_AddStringToObject(p_pat, "description", "Glob pattern to filter files");
      cJSON_AddItemToObject(props, "pattern", p_pat);
      cJSON_AddItemToObject(params, "properties", props);
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("path"));
      cJSON_AddItemToObject(params, "required", req);
      cJSON_AddItemToObject(tool, "parameters", params);
      cJSON_AddItemToArray(tools, tool);
   }

   /* verify */
   {
      cJSON *tool = cJSON_CreateObject();
      cJSON_AddStringToObject(tool, "type", "function");
      cJSON_AddStringToObject(tool, "name", "verify");
      cJSON_AddStringToObject(
          tool, "description",
          "Verify an assertion. check_type: http_status, file_contains, command_succeeds.");
      cJSON *params = cJSON_CreateObject();
      cJSON_AddStringToObject(params, "type", "object");
      cJSON *props = cJSON_CreateObject();
      cJSON *p1 = cJSON_CreateObject();
      cJSON_AddStringToObject(p1, "type", "string");
      cJSON_AddItemToObject(props, "check_type", p1);
      cJSON *p2 = cJSON_CreateObject();
      cJSON_AddStringToObject(p2, "type", "string");
      cJSON_AddItemToObject(props, "target", p2);
      cJSON *p3 = cJSON_CreateObject();
      cJSON_AddStringToObject(p3, "type", "string");
      cJSON_AddItemToObject(props, "expected", p3);
      cJSON_AddItemToObject(params, "properties", props);
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("check_type"));
      cJSON_AddItemToArray(req, cJSON_CreateString("target"));
      cJSON_AddItemToObject(params, "required", req);
      cJSON_AddItemToObject(tool, "parameters", params);
      cJSON_AddItemToArray(tools, tool);
   }

   /* git_log */
   {
      cJSON *tool = cJSON_CreateObject();
      cJSON_AddStringToObject(tool, "type", "function");
      cJSON_AddStringToObject(tool, "name", "git_log");
      cJSON_AddStringToObject(tool, "description", "Show recent git commits for a repository.");
      cJSON *params = cJSON_CreateObject();
      cJSON_AddStringToObject(params, "type", "object");
      cJSON *props = cJSON_CreateObject();
      cJSON *p1 = cJSON_CreateObject();
      cJSON_AddStringToObject(p1, "type", "string");
      cJSON_AddItemToObject(props, "path", p1);
      cJSON *p2 = cJSON_CreateObject();
      cJSON_AddStringToObject(p2, "type", "integer");
      cJSON_AddItemToObject(props, "count", p2);
      cJSON_AddItemToObject(params, "properties", props);
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("path"));
      cJSON_AddItemToObject(params, "required", req);
      cJSON_AddItemToObject(tool, "parameters", params);
      cJSON_AddItemToArray(tools, tool);
   }

   /* grep */
   {
      cJSON *tool = cJSON_CreateObject();
      cJSON_AddStringToObject(tool, "type", "function");
      cJSON_AddStringToObject(tool, "name", "grep");
      cJSON_AddStringToObject(
          tool, "description",
          "Search for a pattern in files. Returns matching lines with file:line.");
      cJSON *params = cJSON_CreateObject();
      cJSON_AddStringToObject(params, "type", "object");
      cJSON *props = cJSON_CreateObject();
      cJSON *p1 = cJSON_CreateObject();
      cJSON_AddStringToObject(p1, "type", "string");
      cJSON_AddItemToObject(props, "path", p1);
      cJSON *p2 = cJSON_CreateObject();
      cJSON_AddStringToObject(p2, "type", "string");
      cJSON_AddItemToObject(props, "pattern", p2);
      cJSON *p3 = cJSON_CreateObject();
      cJSON_AddStringToObject(p3, "type", "integer");
      cJSON_AddItemToObject(props, "max_results", p3);
      cJSON_AddItemToObject(params, "properties", props);
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("path"));
      cJSON_AddItemToArray(req, cJSON_CreateString("pattern"));
      cJSON_AddItemToObject(params, "required", req);
      cJSON_AddItemToObject(tool, "parameters", params);
      cJSON_AddItemToArray(tools, tool);
   }

   /* git_diff */
   {
      cJSON *tool = cJSON_CreateObject();
      cJSON_AddStringToObject(tool, "type", "function");
      cJSON_AddStringToObject(tool, "name", "git_diff");
      cJSON_AddStringToObject(tool, "description",
                              "Show git diff for a repository. Optionally diff against a ref.");
      cJSON *params = cJSON_CreateObject();
      cJSON_AddStringToObject(params, "type", "object");
      cJSON *props = cJSON_CreateObject();
      cJSON *p1 = cJSON_CreateObject();
      cJSON_AddStringToObject(p1, "type", "string");
      cJSON_AddItemToObject(props, "path", p1);
      cJSON *p2 = cJSON_CreateObject();
      cJSON_AddStringToObject(p2, "type", "string");
      cJSON_AddItemToObject(props, "ref", p2);
      cJSON_AddItemToObject(params, "properties", props);
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("path"));
      cJSON_AddItemToObject(params, "required", req);
      cJSON_AddItemToObject(tool, "parameters", params);
      cJSON_AddItemToArray(tools, tool);
   }

   /* git_status */
   {
      cJSON *tool = cJSON_CreateObject();
      cJSON_AddStringToObject(tool, "type", "function");
      cJSON_AddStringToObject(tool, "name", "git_status");
      cJSON_AddStringToObject(tool, "description",
                              "Show git status (porcelain format) for a repository.");
      cJSON *params = cJSON_CreateObject();
      cJSON_AddStringToObject(params, "type", "object");
      cJSON *props = cJSON_CreateObject();
      cJSON *p1 = cJSON_CreateObject();
      cJSON_AddStringToObject(p1, "type", "string");
      cJSON_AddItemToObject(props, "path", p1);
      cJSON_AddItemToObject(params, "properties", props);
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("path"));
      cJSON_AddItemToObject(params, "required", req);
      cJSON_AddItemToObject(tool, "parameters", params);
      cJSON_AddItemToArray(tools, tool);
   }

   /* env_get */
   {
      cJSON *tool = cJSON_CreateObject();
      cJSON_AddStringToObject(tool, "type", "function");
      cJSON_AddStringToObject(tool, "name", "env_get");
      cJSON_AddStringToObject(tool, "description", "Get the value of an environment variable.");
      cJSON *params = cJSON_CreateObject();
      cJSON_AddStringToObject(params, "type", "object");
      cJSON *props = cJSON_CreateObject();
      cJSON *p1 = cJSON_CreateObject();
      cJSON_AddStringToObject(p1, "type", "string");
      cJSON_AddItemToObject(props, "name", p1);
      cJSON_AddItemToObject(params, "properties", props);
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("name"));
      cJSON_AddItemToObject(params, "required", req);
      cJSON_AddItemToObject(tool, "parameters", params);
      cJSON_AddItemToArray(tools, tool);
   }

   /* test */
   {
      cJSON *tool = cJSON_CreateObject();
      cJSON_AddStringToObject(tool, "type", "function");
      cJSON_AddStringToObject(tool, "name", "test");
      cJSON_AddStringToObject(tool, "description",
                              "Check file/dir existence, type, and permissions. "
                              "check: exists, is_file, is_dir, readable, writable, executable.");
      cJSON *params = cJSON_CreateObject();
      cJSON_AddStringToObject(params, "type", "object");
      cJSON *props = cJSON_CreateObject();
      cJSON *p1 = cJSON_CreateObject();
      cJSON_AddStringToObject(p1, "type", "string");
      cJSON_AddItemToObject(props, "path", p1);
      cJSON *p2 = cJSON_CreateObject();
      cJSON_AddStringToObject(p2, "type", "string");
      cJSON_AddItemToObject(props, "check", p2);
      cJSON_AddItemToObject(params, "properties", props);
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("path"));
      cJSON_AddItemToObject(params, "required", req);
      cJSON_AddItemToObject(tool, "parameters", params);
      cJSON_AddItemToArray(tools, tool);
   }

   return tools;
}

/* Build tools array in Anthropic format by converting from OpenAI format.
 * OpenAI: {"type":"function","function":{"name":"...","description":"...","parameters":{...}}}
 * Anthropic: {"name":"...","description":"...","input_schema":{...}} */
cJSON *build_tools_array_anthropic(void)
{
   cJSON *openai_tools = build_tools_array();
   cJSON *tools = cJSON_CreateArray();

   int n = cJSON_GetArraySize(openai_tools);
   for (int i = 0; i < n; i++)
   {
      cJSON *oi = cJSON_GetArrayItem(openai_tools, i);
      cJSON *fn = cJSON_GetObjectItem(oi, "function");
      if (!fn)
         continue;

      cJSON *tool = cJSON_CreateObject();
      cJSON *name = cJSON_GetObjectItem(fn, "name");
      cJSON *desc = cJSON_GetObjectItem(fn, "description");
      cJSON *params = cJSON_GetObjectItem(fn, "parameters");

      if (name && cJSON_IsString(name))
         cJSON_AddStringToObject(tool, "name", name->valuestring);
      if (desc && cJSON_IsString(desc))
         cJSON_AddStringToObject(tool, "description", desc->valuestring);
      if (params)
         cJSON_AddItemToObject(tool, "input_schema", cJSON_Duplicate(params, 1));

      cJSON_AddItemToArray(tools, tool);
   }

   cJSON_Delete(openai_tools);
   return tools;
}
