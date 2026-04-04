/* util.c: core utilities (normalization, option parsing, security, path helpers) */
#include "aimee.h"
#include <ctype.h>
#include <regex.h>
#include <stdarg.h>
#include <time.h>

#include <sys/wait.h>

#ifndef _WIN32
#include <signal.h>
#include <unistd.h>
#endif

/* --- Shared utilities (used by all modules including tests) --- */

char *safe_strdup(const char *s)
{
   if (!s)
      return NULL;
   char *dup = strdup(s);
   if (!dup)
      fatal("out of memory (strdup)");
   return dup;
}

void fatal(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   fprintf(stderr, "aimee: ");
   vfprintf(stderr, fmt, ap);
   fprintf(stderr, "\n");
   va_end(ap);
   exit(1);
}

void now_utc(char *buf, size_t len)
{
   time_t t = time(NULL);
   struct tm tm;
   gmtime_r(&t, &tm);
   strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

/* --- Filler words for normalization --- */

static const char *filler_words[] = {"the", "a", "an", "is", "are", "was", "were", "be", NULL};

static int is_filler(const char *word)
{
   for (int i = 0; filler_words[i]; i++)
   {
      if (strcmp(word, filler_words[i]) == 0)
         return 1;
   }
   return 0;
}

/* --- normalize_key --- */

char *normalize_key(const char *key, char *buf, size_t buf_len)
{
   if (!key || !buf || buf_len == 0)
   {
      if (buf && buf_len > 0)
         buf[0] = '\0';
      return buf;
   }

   /* Lowercase copy */
   char tmp[4096];
   size_t ki = 0;
   for (size_t i = 0; key[i] && ki < sizeof(tmp) - 1; i++)
   {
      tmp[ki++] = (char)tolower((unsigned char)key[i]);
   }
   tmp[ki] = '\0';

   /* Split into words, skip filler, collapse whitespace */
   size_t out = 0;
   char *saveptr;
   char *tok = strtok_r(tmp, " \t\n\r", &saveptr);
   while (tok)
   {
      if (!is_filler(tok))
      {
         if (out > 0 && out < buf_len - 1)
            buf[out++] = ' ';
         size_t wlen = strlen(tok);
         if (out + wlen < buf_len)
         {
            memcpy(buf + out, tok, wlen);
            out += wlen;
         }
      }
      tok = strtok_r(NULL, " \t\n\r", &saveptr);
   }
   buf[out] = '\0';
   return buf;
}

/* --- shlex_split --- */

int shlex_split(const char *command, char **out, int max_tokens)
{
   if (!command || !out || max_tokens <= 0)
      return 0;

   int count = 0;
   const char *p = command;

   while (*p && count < max_tokens)
   {
      /* Skip whitespace */
      while (*p == ' ' || *p == '\t')
         p++;
      if (!*p)
         break;

      /* Check for digit-prefixed redirections like 2> */
      if (*p == '2' && p[1] == '>')
      {
         char op[4] = {'2', '>', '\0', '\0'};
         if (p[2] == '>')
         {
            op[2] = '>';
            p += 3;
         }
         else
         {
            p += 2;
         }
         out[count] = strdup(op);
         if (!out[count])
            break;
         count++;
         continue;
      }

      /* Check for shell operators */
      if (*p == '|' || *p == '>' || *p == '<' || *p == '&' || *p == ';')
      {
         char op[4] = {*p, '\0', '\0', '\0'};
         if (p[1] == p[0]) /* ||, >>, &&, etc. */
         {
            op[1] = p[1];
            p += 2;
         }
         else if (*p == '>' && p[1] == '&')
         {
            op[1] = '&';
            p += 2;
         }
         else
         {
            p++;
         }
         out[count] = strdup(op);
         if (!out[count])
            break;
         count++;
         continue;
      }

      /* Build token */
      char token[MAX_PATH_LEN];
      size_t ti = 0;
      char quote = 0;

      while (*p && ti < sizeof(token) - 1)
      {
         if (quote)
         {
            if (*p == quote)
            {
               quote = 0;
               p++;
            }
            else if (*p == '\\' && quote == '"' && p[1])
            {
               p++;
               token[ti++] = *p++;
            }
            else
            {
               token[ti++] = *p++;
            }
         }
         else
         {
            if (*p == '\'' || *p == '"')
            {
               quote = *p++;
            }
            else if (*p == '\\' && p[1])
            {
               p++;
               token[ti++] = *p++;
            }
            else if (*p == ' ' || *p == '\t' || *p == '|' || *p == '>' || *p == '<' || *p == '&' ||
                     *p == ';')
            {
               break;
            }
            else
            {
               token[ti++] = *p++;
            }
         }
      }
      token[ti] = '\0';

      if (ti > 0)
      {
         out[count] = strdup(token);
         if (!out[count])
            break;
         count++;
      }
   }

   return count;
}

/* --- is_likely_path --- */

int is_likely_path(const char *tok)
{
   if (!tok)
      return 0;
   if (tok[0] == '/')
      return 1;
   if (tok[0] == '.' && tok[1] == '/')
      return 1;
   if (tok[0] == '.' && tok[1] == '.' && tok[2] == '/')
      return 1;
   if (tok[0] == '~' && tok[1] == '/')
      return 1;

   /* Check for file extension patterns */
   const char *dot = strrchr(tok, '.');
   if (dot && strchr(tok, '/'))
      return 1;

   return 0;
}

/* --- extract_paths_shlex --- */

int extract_paths_shlex(const char *command, char **out, int max_paths)
{
   char *tokens[256];
   int tc = shlex_split(command, tokens, 256);

   int count = 0;
   for (int i = 0; i < tc && count < max_paths; i++)
   {
      if (is_likely_path(tokens[i]))
      {
         out[count] = strdup(tokens[i]);
         count++;
      }
   }

   /* Free all tokens */
   for (int i = 0; i < tc; i++)
      free(tokens[i]);

   return count;
}

/* --- Option parsing --- */

static int is_bool_flag(const char *name, const char **bool_flags)
{
   if (!bool_flags)
      return 0;
   for (int i = 0; bool_flags[i]; i++)
   {
      if (strcmp(name, bool_flags[i]) == 0)
         return 1;
   }
   return 0;
}

void opt_parse(int argc, char **argv, const char **bool_flags, opt_parsed_t *out)
{
   memset(out, 0, sizeof(*out));

   for (int i = 0; i < argc; i++)
   {
      if (strncmp(argv[i], "--", 2) == 0 && argv[i][2] != '\0')
      {
         const char *flag = argv[i] + 2;
         const char *eq = strchr(flag, '=');

         if (out->flag_count >= OPT_MAX_FLAGS)
            continue;

         opt_flag_t *f = &out->flags[out->flag_count];
         if (eq)
         {
            /* --flag=value: name includes chars up to '=' */
            f->name = flag;
            f->value = eq + 1;
            f->is_bool = 0;
         }
         else if (is_bool_flag(flag, bool_flags))
         {
            f->name = flag;
            f->value = "";
            f->is_bool = 1;
         }
         else if (i + 1 < argc && argv[i + 1][0] != '-')
         {
            /* --flag value */
            f->name = flag;
            f->value = argv[++i];
            f->is_bool = 0;
         }
         else
         {
            /* Treat as bool */
            f->name = flag;
            f->value = "";
            f->is_bool = 1;
         }
         out->flag_count++;
      }
      else
      {
         if (out->pos_count < OPT_MAX_POSITIONAL)
            out->positional[out->pos_count++] = argv[i];
      }
   }
}

const char *opt_get(const opt_parsed_t *opts, const char *name)
{
   size_t nlen = strlen(name);
   for (int i = 0; i < opts->flag_count; i++)
   {
      const char *fn = opts->flags[i].name;
      if (strncmp(fn, name, nlen) == 0 && (fn[nlen] == '\0' || fn[nlen] == '='))
         return opts->flags[i].value;
   }
   return NULL;
}

int opt_has(const opt_parsed_t *opts, const char *name)
{
   return opt_get(opts, name) != NULL;
}

const char *opt_pos(const opt_parsed_t *opts, int index)
{
   if (index < 0 || index >= opts->pos_count)
      return NULL;
   return opts->positional[index];
}

int opt_get_int(const opt_parsed_t *opts, const char *name, int default_val)
{
   const char *v = opt_get(opts, name);
   return v ? atoi(v) : default_val;
}

/* --- Security helpers --- */

#ifndef _WIN32

int safe_exec_capture(const char *const argv[], char **out_buf, size_t max_out)
{
   *out_buf = NULL;
   if (!argv || !argv[0])
      return -1;

   int pipefd[2];
   if (pipe(pipefd) != 0)
      return -1;

   pid_t pid = fork();
   if (pid < 0)
   {
      close(pipefd[0]);
      close(pipefd[1]);
      return -1;
   }

   if (pid == 0)
   {
      /* Child: redirect stdout to pipe, close stdin/stderr */
      close(pipefd[0]);
      dup2(pipefd[1], STDOUT_FILENO);
      dup2(pipefd[1], STDERR_FILENO);
      close(pipefd[1]);
      execvp(argv[0], (char *const *)argv);
      _exit(127);
   }

   /* Parent */
   close(pipefd[1]);

   char *buf = malloc(max_out + 1);
   if (!buf)
   {
      close(pipefd[0]);
      kill(pid, SIGKILL);
      waitpid(pid, NULL, 0);
      return -1;
   }

   size_t total = 0;
   while (total < max_out)
   {
      ssize_t n = read(pipefd[0], buf + total, max_out - total);
      if (n <= 0)
         break;
      total += (size_t)n;
   }
   close(pipefd[0]);
   buf[total] = '\0';

   int status = 0;
   waitpid(pid, &status, 0);

   *out_buf = buf;
   return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

#else

int safe_exec_capture(const char *const argv[], char **out_buf, size_t max_out)
{
   (void)argv;
   (void)max_out;
   *out_buf = NULL;
   return -1;
}

#endif /* _WIN32 */

/* --- Command execution --- */

#define RUN_CMD_BUF_SIZE 65536

char *run_cmd(const char *cmd, int *exit_code)
{
   FILE *fp = popen(cmd, "r");
   if (!fp)
   {
      *exit_code = -1;
      return NULL;
   }

   char *buf = malloc(RUN_CMD_BUF_SIZE);
   if (!buf)
   {
      pclose(fp);
      *exit_code = -1;
      return NULL;
   }

   size_t total = 0;
   size_t n;
   while ((n = fread(buf + total, 1, RUN_CMD_BUF_SIZE - total - 1, fp)) > 0)
      total += n;
   buf[total] = '\0';

   int status = pclose(fp);
   *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
   return buf;
}

/* --- Regex helpers --- */

int regex_match(const char *pattern, const char *text, int flags)
{
   if (!pattern || !text)
      return 0;
   regex_t re;
   if (regcomp(&re, pattern, flags | REG_NOSUB) != 0)
      return 0;
   int matched = (regexec(&re, text, 0, NULL, 0) == 0);
   regfree(&re);
   return matched;
}

int has_shell_metachar(const char *s)
{
   if (!s)
      return 0;
   for (; *s; s++)
   {
      switch (*s)
      {
      case ';':
      case '|':
      case '&':
      case '$':
      case '`':
      case '(':
      case ')':
      case '{':
      case '}':
      case '<':
      case '>':
      case '\n':
      case '\r':
      case '\'':
      case '"':
      case '\\':
         return 1;
      }
   }
   return 0;
}

char *shell_escape(const char *raw)
{
   if (!raw)
      return strdup("");
   size_t len = strlen(raw);
   char *esc = malloc(len * 4 + 1);
   if (!esc)
      return strdup("");
   size_t j = 0;
   for (size_t i = 0; i < len; i++)
   {
      if (raw[i] == '\'')
      {
         esc[j++] = '\'';
         esc[j++] = '\\';
         esc[j++] = '\'';
         esc[j++] = '\'';
      }
      else
      {
         esc[j++] = raw[i];
      }
   }
   esc[j] = '\0';
   return esc;
}

int is_valid_pkg_name(const char *s)
{
   if (!s || !*s)
      return 0;
   /* First char must be alphanumeric */
   if (!isalnum((unsigned char)s[0]))
      return 0;
   for (int i = 1; s[i]; i++)
   {
      char c = s[i];
      if (isalnum((unsigned char)c) || c == '.' || c == '+' || c == '-' || c == ':')
         continue;
      return 0;
   }
   return 1;
}

int is_valid_git_url(const char *url)
{
   if (!url)
      return 0;
   return strncmp(url, "https://", 8) == 0 || strncmp(url, "http://", 7) == 0 ||
          strncmp(url, "git@", 4) == 0 || strncmp(url, "ssh://", 6) == 0 ||
          strncmp(url, "file://", 7) == 0;
}

void sanitize_shell_token(char *s)
{
   if (!s)
      return;
   for (; *s; s++)
   {
      if (isalnum((unsigned char)*s) || *s == '-' || *s == '_' || *s == '.')
         continue;
      *s = '_';
   }
}

int is_safe_id(const char *s)
{
   if (!s || !s[0] || strlen(s) > 128)
      return 0;
   for (const char *p = s; *p; p++)
   {
      if (isalnum((unsigned char)*p) || *p == '-' || *p == '_')
         continue;
      return 0;
   }
   return 1;
}
