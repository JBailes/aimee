/* server_compute.c: compute-layer handlers (tool.execute, delegate, chat.send_stream) */
#define _GNU_SOURCE
#include "aimee.h"
#include "server.h"
#include "compute_pool.h"
#include "agent.h"
#include "cJSON.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#define CLAUDE_LINE_MAX          (256 * 1024)
#define DELEGATION_INPUT_TIMEOUT 60 /* seconds */
#define MAX_ACTIVE_DELEGATIONS   32

/* Delegation mailbox: allows delegates to pause and receive parent replies */
typedef struct
{
   char delegation_id[64];
   pthread_mutex_t lock;
   pthread_cond_t reply_ready;
   char reply[4096];
   int has_reply;
   int active;
} delegation_mailbox_t;

static delegation_mailbox_t g_mailboxes[MAX_ACTIVE_DELEGATIONS];
static pthread_mutex_t g_mailbox_lock = PTHREAD_MUTEX_INITIALIZER;

static delegation_mailbox_t *mailbox_acquire(const char *delegation_id)
{
   pthread_mutex_lock(&g_mailbox_lock);
   for (int i = 0; i < MAX_ACTIVE_DELEGATIONS; i++)
   {
      if (!g_mailboxes[i].active)
      {
         delegation_mailbox_t *mb = &g_mailboxes[i];
         snprintf(mb->delegation_id, sizeof(mb->delegation_id), "%s", delegation_id);
         pthread_mutex_init(&mb->lock, NULL);
         pthread_cond_init(&mb->reply_ready, NULL);
         mb->reply[0] = '\0';
         mb->has_reply = 0;
         mb->active = 1;
         pthread_mutex_unlock(&g_mailbox_lock);
         return mb;
      }
   }
   pthread_mutex_unlock(&g_mailbox_lock);
   return NULL;
}

static void mailbox_release(delegation_mailbox_t *mb)
{
   if (!mb)
      return;
   pthread_mutex_lock(&g_mailbox_lock);
   mb->active = 0;
   mb->delegation_id[0] = '\0';
   pthread_mutex_destroy(&mb->lock);
   pthread_cond_destroy(&mb->reply_ready);
   pthread_mutex_unlock(&g_mailbox_lock);
}

static delegation_mailbox_t *mailbox_find(const char *delegation_id)
{
   pthread_mutex_lock(&g_mailbox_lock);
   for (int i = 0; i < MAX_ACTIVE_DELEGATIONS; i++)
   {
      if (g_mailboxes[i].active && strcmp(g_mailboxes[i].delegation_id, delegation_id) == 0)
      {
         delegation_mailbox_t *mb = &g_mailboxes[i];
         pthread_mutex_unlock(&g_mailbox_lock);
         return mb;
      }
   }
   pthread_mutex_unlock(&g_mailbox_lock);
   return NULL;
}

/* Wait for a parent reply with timeout. Returns 0 on reply, -1 on timeout. */
static int mailbox_wait(delegation_mailbox_t *mb, char *out, size_t out_len, int timeout_secs)
{
   struct timespec ts;
   clock_gettime(CLOCK_REALTIME, &ts);
   ts.tv_sec += timeout_secs;

   pthread_mutex_lock(&mb->lock);
   while (!mb->has_reply)
   {
      int rc = pthread_cond_timedwait(&mb->reply_ready, &mb->lock, &ts);
      if (rc != 0) /* ETIMEDOUT or error */
      {
         pthread_mutex_unlock(&mb->lock);
         return -1;
      }
   }
   snprintf(out, out_len, "%s", mb->reply);
   mb->has_reply = 0;
   mb->reply[0] = '\0';
   pthread_mutex_unlock(&mb->lock);
   return 0;
}

/* Send a reply to a waiting delegate. */
static void mailbox_reply(delegation_mailbox_t *mb, const char *content)
{
   pthread_mutex_lock(&mb->lock);
   snprintf(mb->reply, sizeof(mb->reply), "%s", content);
   mb->has_reply = 1;
   pthread_cond_signal(&mb->reply_ready);
   pthread_mutex_unlock(&mb->lock);
}

/* Record a delegation message in the database */
static void deleg_msg_record(sqlite3 *db, const char *delegation_id, const char *direction,
                             const char *content)
{
   if (!db)
      return;
   static const char *sql = "INSERT INTO delegation_messages (delegation_id, direction, content)"
                            " VALUES (?, ?, ?)";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return;
   sqlite3_bind_text(stmt, 1, delegation_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, direction, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, content, -1, SQLITE_TRANSIENT);
   DB_STEP_LOG(stmt, "deleg_msg_record");
   sqlite3_reset(stmt);
}

/* Global: current delegation mailbox for the calling thread (used by tool_request_input) */
static __thread delegation_mailbox_t *tl_mailbox = NULL;
static __thread sqlite3 *tl_deleg_db = NULL;

/* Called by tool_request_input from within the delegate agent */
char *delegation_request_input(const char *question)
{
   if (!tl_mailbox || !tl_mailbox->active)
      return NULL;

   /* Record the question */
   deleg_msg_record(tl_deleg_db, tl_mailbox->delegation_id, "delegate_to_parent", question);

   /* Wait for parent reply */
   char reply[4096];
   if (mailbox_wait(tl_mailbox, reply, sizeof(reply), DELEGATION_INPUT_TIMEOUT) != 0)
      return NULL; /* timeout */

   /* Record the reply */
   deleg_msg_record(tl_deleg_db, tl_mailbox->delegation_id, "parent_to_delegate", reply);

   size_t len = strlen(reply);
   char *out = malloc(len + 1);
   if (out)
      memcpy(out, reply, len + 1);
   return out;
}

/* Context for async compute work */
typedef struct
{
   server_ctx_t *server;
   int conn_fd;
   sqlite3 *db;
   pthread_mutex_t *write_mutex;
   cJSON *req; /* owned by this context */
} compute_ctx_t;

/* Write all data to fd, handling non-blocking with poll */
static int write_all(int fd, const char *data, size_t len)
{
   size_t total = 0;
   while (total < len)
   {
      ssize_t n = write(fd, data + total, len - total);
      if (n > 0)
      {
         total += (size_t)n;
      }
      else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      {
         struct pollfd pfd = {.fd = fd, .events = POLLOUT};
         if (poll(&pfd, 1, 30000) <= 0)
            return -1;
      }
      else
      {
         return -1;
      }
   }
   return 0;
}

/* Write response and free context */
static void compute_respond(compute_ctx_t *cctx, cJSON *resp)
{
   char *json_str = cJSON_PrintUnformatted(resp);
   if (json_str)
   {
      size_t len = strlen(json_str);
      pthread_mutex_lock(cctx->write_mutex);
      if (write_all(cctx->conn_fd, json_str, len) == 0)
         write_all(cctx->conn_fd, "\n", 1);
      pthread_mutex_unlock(cctx->write_mutex);
      free(json_str);
   }
   cJSON_Delete(resp);
}

static void compute_error(compute_ctx_t *cctx, const char *message)
{
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "error");
   cJSON_AddStringToObject(resp, "message", message);
   compute_respond(cctx, resp);
}

static void compute_ctx_free(compute_ctx_t *cctx)
{
   if (cctx->req)
      cJSON_Delete(cctx->req);
   if (cctx->db)
      sqlite3_close(cctx->db);
   if (cctx->write_mutex)
   {
      pthread_mutex_destroy(cctx->write_mutex);
      free(cctx->write_mutex);
   }
   free(cctx);
}

/* --- tool.execute worker --- */

static void tool_execute_worker(void *arg)
{
   compute_ctx_t *cctx = (compute_ctx_t *)arg;
   cJSON *req = cctx->req;

   cJSON *jtool = cJSON_GetObjectItemCaseSensitive(req, "tool");
   cJSON *jargs = cJSON_GetObjectItemCaseSensitive(req, "arguments");
   cJSON *jcwd = cJSON_GetObjectItemCaseSensitive(req, "cwd");
   cJSON *jsid = cJSON_GetObjectItemCaseSensitive(req, "session_id");
   cJSON *jtimeout = cJSON_GetObjectItemCaseSensitive(req, "timeout_ms");

   const char *tool = cJSON_IsString(jtool) ? jtool->valuestring : "";
   const char *args = cJSON_IsString(jargs) ? jargs->valuestring : "{}";
   const char *cwd = cJSON_IsString(jcwd) ? jcwd->valuestring : "";
   const char *sid = cJSON_IsString(jsid) ? jsid->valuestring : "unknown";
   int timeout_ms = cJSON_IsNumber(jtimeout) ? (int)jtimeout->valuedouble : 30000;

   if (!tool[0])
   {
      compute_error(cctx, "missing tool");
      compute_ctx_free(cctx);
      return;
   }
   if (!is_safe_id(sid))
   {
      compute_error(cctx, "invalid session_id");
      compute_ctx_free(cctx);
      return;
   }

   /* Change to requested cwd for tool execution (validate: absolute, no traversal) */
   char old_cwd[MAX_PATH_LEN];
   if (getcwd(old_cwd, sizeof(old_cwd)) && cwd[0] && cwd[0] == '/' && !strstr(cwd, "/../") &&
       !strstr(cwd, "/.."))
      (void)chdir(cwd);

   /* Guardrail pre-check */
   config_t cfg;
   config_load(&cfg);

   char state_path[MAX_PATH_LEN];
   snprintf(state_path, sizeof(state_path), "%s/session-%s.state", config_output_dir(), sid);
   session_state_t state;
   session_state_load(&state, state_path);

   char msg[1024] = "";
   int rc = pre_tool_check(cctx->db, tool, args, &state, config_guardrail_mode(&cfg), cwd, msg,
                           sizeof(msg));
   session_state_save(&state, state_path);

   if (rc != 0)
   {
      cJSON *resp = cJSON_CreateObject();
      cJSON_AddStringToObject(resp, "status", "blocked");
      cJSON_AddStringToObject(resp, "message", msg);
      cJSON_AddNumberToObject(resp, "exit_code", rc);
      compute_respond(cctx, resp);
      if (old_cwd[0])
         (void)chdir(old_cwd);
      compute_ctx_free(cctx);
      return;
   }

   /* Execute tool */
   char *result = dispatch_tool_call(tool, args, timeout_ms);

   /* Restore cwd */
   if (old_cwd[0])
      (void)chdir(old_cwd);

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "result", result ? result : "");
   free(result);
   compute_respond(cctx, resp);
   compute_ctx_free(cctx);
}

/* --- delegate worker --- */

static void delegate_worker(void *arg)
{
   compute_ctx_t *cctx = (compute_ctx_t *)arg;
   cJSON *req = cctx->req;

   cJSON *jrole = cJSON_GetObjectItemCaseSensitive(req, "role");
   cJSON *jprompt = cJSON_GetObjectItemCaseSensitive(req, "prompt");
   cJSON *jmax = cJSON_GetObjectItemCaseSensitive(req, "max_tokens");
   cJSON *jsystem = cJSON_GetObjectItemCaseSensitive(req, "system_prompt");
   cJSON *jid = cJSON_GetObjectItemCaseSensitive(req, "delegation_id");
   cJSON *jsid = cJSON_GetObjectItemCaseSensitive(req, "session_id");
   cJSON *jcwd = cJSON_GetObjectItemCaseSensitive(req, "cwd");

   const char *role = cJSON_IsString(jrole) ? jrole->valuestring : "execute";
   const char *prompt = cJSON_IsString(jprompt) ? jprompt->valuestring : "";
   int max_tokens = cJSON_IsNumber(jmax) ? (int)jmax->valuedouble : 4096;
   const char *system_prompt = cJSON_IsString(jsystem) ? jsystem->valuestring : NULL;
   const char *sid = cJSON_IsString(jsid) ? jsid->valuestring : NULL;
   const char *cwd = cJSON_IsString(jcwd) ? jcwd->valuestring : "";

   /* Generate delegation ID if not provided */
   char deleg_id[64];
   if (cJSON_IsString(jid) && jid->valuestring[0])
      snprintf(deleg_id, sizeof(deleg_id), "%s", jid->valuestring);
   else
      snprintf(deleg_id, sizeof(deleg_id), "deleg-%d-%ld", (int)getpid(), (long)time(NULL));

   if (!prompt[0])
   {
      compute_error(cctx, "missing prompt");
      compute_ctx_free(cctx);
      return;
   }

   /* Load agent config */
   agent_config_t acfg;
   if (agent_load_config(&acfg) != 0)
   {
      compute_error(cctx, "failed to load agent config");
      compute_ctx_free(cctx);
      return;
   }

   /* Build system prompt if not provided */
   char sys_buf[4096] = "";
   if (!system_prompt)
   {
      snprintf(sys_buf, sizeof(sys_buf),
               "You are a sub-agent executing a delegated task. "
               "Complete the task and report results. "
               "If you need input from the parent agent, use the request_input tool.");
      system_prompt = sys_buf;
   }

   /* Change to requested cwd (validate: absolute, no traversal) */
   char old_cwd[MAX_PATH_LEN];
   old_cwd[0] = '\0';
   if (getcwd(old_cwd, sizeof(old_cwd)) && cwd[0] && cwd[0] == '/' && !strstr(cwd, "/../") &&
       !strstr(cwd, "/.."))
      (void)chdir(cwd);

   /* Set up delegation mailbox for this thread */
   delegation_mailbox_t *mb = mailbox_acquire(deleg_id);
   tl_mailbox = mb;
   tl_deleg_db = cctx->db;
   session_id_set_override(sid);

   /* Run agent with tools — delegates always get tool access regardless of
    * the agent config's tools_enabled flag. */
   agent_result_t result;
   memset(&result, 0, sizeof(result));
   /* NOTE: agent_http_init() (SSL_CTX setup) is called once in server_main.c.
    * Do NOT call it here — the global SSL_CTX is shared across threads. */

   int rc = agent_run_with_tools(cctx->db, &acfg, role, system_prompt, prompt, max_tokens, &result);

   /* Clean up mailbox */
   session_id_clear_override();
   tl_mailbox = NULL;
   tl_deleg_db = NULL;
   mailbox_release(mb);

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "delegation_id", deleg_id);
   if (rc == 0)
   {
      cJSON_AddStringToObject(resp, "status", "ok");
      cJSON_AddStringToObject(resp, "response", result.response ? result.response : "");
      cJSON_AddNumberToObject(resp, "turns", result.turns);
      cJSON_AddNumberToObject(resp, "tool_calls", result.tool_calls);
      cJSON_AddNumberToObject(resp, "confidence", result.confidence);
      cJSON_AddNumberToObject(resp, "latency_ms", result.latency_ms);
      cJSON_AddStringToObject(resp, "agent", result.agent_name);
   }
   else
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message",
                              result.error[0] ? result.error : "delegation failed");
      if (result.response)
         cJSON_AddStringToObject(resp, "response", result.response);
   }

   free(result.response);
   compute_respond(cctx, resp);

   /* Restore cwd */
   if (old_cwd[0])
      (void)chdir(old_cwd);

   compute_ctx_free(cctx);
}

/* --- Public handlers (called from server dispatch) --- */

static compute_ctx_t *create_compute_ctx(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   compute_ctx_t *cctx = calloc(1, sizeof(compute_ctx_t));
   if (!cctx)
      return NULL;

   cctx->server = ctx;
   cctx->conn_fd = conn->fd;

   /* Clone the request since the original will be freed after dispatch */
   cctx->req = cJSON_Duplicate(req, 1);

   /* Open a separate DB connection for the worker thread */
   config_t cfg;
   config_load(&cfg);
   cctx->db = db_open_fast(cfg.db_path);

   /* Create per-context write mutex */
   cctx->write_mutex = malloc(sizeof(pthread_mutex_t));
   pthread_mutex_init(cctx->write_mutex, NULL);

   return cctx;
}

int handle_tool_execute(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   compute_ctx_t *cctx = create_compute_ctx(ctx, conn, req);
   if (!cctx)
      return server_send_error(conn, "out of memory", NULL);

   if (compute_pool_submit(&ctx->pool, tool_execute_worker, cctx) != 0)
   {
      compute_ctx_free(cctx);
      return server_send_error(conn, "compute queue full", NULL);
   }

   return 0; /* Response will be sent by worker thread */
}

int handle_delegate(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   compute_ctx_t *cctx = create_compute_ctx(ctx, conn, req);
   if (!cctx)
      return server_send_error(conn, "out of memory", NULL);

   if (compute_pool_submit(&ctx->pool, delegate_worker, cctx) != 0)
   {
      compute_ctx_free(cctx);
      return server_send_error(conn, "compute queue full", NULL);
   }

   return 0; /* Response will be sent by worker thread */
}

int handle_delegate_reply(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   cJSON *jid = cJSON_GetObjectItemCaseSensitive(req, "delegation_id");
   cJSON *jcontent = cJSON_GetObjectItemCaseSensitive(req, "content");

   if (!cJSON_IsString(jid) || !cJSON_IsString(jcontent))
      return server_send_error(conn, "missing delegation_id or content", NULL);

   delegation_mailbox_t *mb = mailbox_find(jid->valuestring);
   if (!mb)
      return server_send_error(conn, "no active delegation with that ID", NULL);

   mailbox_reply(mb, jcontent->valuestring);

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   return server_send_response(conn, resp);
}

/* --- chat.send_stream worker --- */

/* Send a streaming event as newline-delimited JSON */
static void stream_event(compute_ctx_t *cctx, const char *event, const char *key, const char *value)
{
   cJSON *evt = cJSON_CreateObject();
   cJSON_AddStringToObject(evt, "event", event);
   if (key && value)
      cJSON_AddStringToObject(evt, key, value);

   char *json_str = cJSON_PrintUnformatted(evt);
   cJSON_Delete(evt);
   if (json_str)
   {
      pthread_mutex_lock(cctx->write_mutex);
      write_all(cctx->conn_fd, json_str, strlen(json_str));
      write_all(cctx->conn_fd, "\n", 1);
      pthread_mutex_unlock(cctx->write_mutex);
      free(json_str);
   }
}

static void chat_stream_worker(void *arg)
{
   compute_ctx_t *cctx = (compute_ctx_t *)arg;
   cJSON *req = cctx->req;

   cJSON *jmsg = cJSON_GetObjectItemCaseSensitive(req, "message");
   cJSON *jcsid = cJSON_GetObjectItemCaseSensitive(req, "claude_session_id");

   const char *message = cJSON_IsString(jmsg) ? jmsg->valuestring : "";
   const char *claude_sid = cJSON_IsString(jcsid) ? jcsid->valuestring : "";

   if (!message[0])
   {
      compute_error(cctx, "missing message");
      compute_ctx_free(cctx);
      return;
   }

   /* Build claude command */
   char cmd[2048];
   char claude_base[1024];

   /* Check for system prompt file */
   char sys_path[MAX_PATH_LEN];
   snprintf(sys_path, sizeof(sys_path), "%s/webchat_system_prompt.txt", config_default_dir());
   int has_sys = (access(sys_path, R_OK) == 0);

   snprintf(claude_base, sizeof(claude_base),
            "claude -p --output-format stream-json --verbose"
            " --include-partial-messages"
            " --allowedTools 'Bash(*)' Edit Read Write Glob Grep"
            " WebFetch WebSearch NotebookEdit"
            " --disallowedTools AskUserQuestion,Agent,RemoteTrigger"
            "%s%s",
            has_sys ? " --append-system-prompt-file " : "", has_sys ? sys_path : "");

   if (claude_sid[0])
   {
      char safe_sid[256];
      snprintf(safe_sid, sizeof(safe_sid), "%s", claude_sid);
      sanitize_shell_token(safe_sid);
      snprintf(cmd, sizeof(cmd), "%s --resume '%s'", claude_base, safe_sid);
   }
   else
      snprintf(cmd, sizeof(cmd), "%s", claude_base);

   /* Create pipes */
   int in_pipe[2], out_pipe[2];
   if (pipe(in_pipe) < 0 || pipe(out_pipe) < 0)
   {
      compute_error(cctx, "pipe failed");
      compute_ctx_free(cctx);
      return;
   }

   pid_t pid = fork();
   if (pid < 0)
   {
      close(in_pipe[0]);
      close(in_pipe[1]);
      close(out_pipe[0]);
      close(out_pipe[1]);
      compute_error(cctx, "fork failed");
      compute_ctx_free(cctx);
      return;
   }

   if (pid == 0)
   {
      /* Child */
      close(in_pipe[1]);
      close(out_pipe[0]);
      dup2(in_pipe[0], STDIN_FILENO);
      dup2(out_pipe[1], STDOUT_FILENO);
      close(in_pipe[0]);
      close(out_pipe[1]);
      int devnull = open("/dev/null", O_WRONLY);
      if (devnull >= 0)
      {
         dup2(devnull, STDERR_FILENO);
         close(devnull);
      }
      execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
      _exit(127);
   }

   /* Parent */
   close(in_pipe[0]);
   close(out_pipe[1]);

   /* Write message to stdin */
   size_t msg_len = strlen(message);
   size_t written = 0;
   while (written < msg_len)
   {
      ssize_t w = write(in_pipe[1], message + written, msg_len - written);
      if (w <= 0)
         break;
      written += (size_t)w;
   }
   close(in_pipe[1]);

   /* Read stream-json output */
   FILE *fp = fdopen(out_pipe[0], "r");
   if (!fp)
   {
      close(out_pipe[0]);
      kill(pid, SIGKILL);
      waitpid(pid, NULL, 0);
      compute_error(cctx, "internal error");
      compute_ctx_free(cctx);
      return;
   }

   char *line = malloc(CLAUDE_LINE_MAX);
   if (!line)
   {
      fclose(fp);
      kill(pid, SIGKILL);
      waitpid(pid, NULL, 0);
      compute_error(cctx, "out of memory");
      compute_ctx_free(cctx);
      return;
   }

   int had_tool_result = 0;
   int first_message = 1;

   while (fgets(line, CLAUDE_LINE_MAX, fp))
   {
      cJSON *obj = cJSON_Parse(line);
      if (!obj)
         continue;

      cJSON *type_j = cJSON_GetObjectItem(obj, "type");
      const char *type = (type_j && cJSON_IsString(type_j)) ? type_j->valuestring : "";

      if (strcmp(type, "stream_event") == 0)
      {
         cJSON *event = cJSON_GetObjectItem(obj, "event");
         if (event)
         {
            cJSON *etype = cJSON_GetObjectItem(event, "type");
            const char *et = (etype && cJSON_IsString(etype)) ? etype->valuestring : "";

            if (strcmp(et, "message_start") == 0)
            {
               if (first_message || had_tool_result)
               {
                  stream_event(cctx, "turn_start", NULL, NULL);
                  first_message = 0;
               }
               had_tool_result = 0;
            }
            else if (strcmp(et, "content_block_delta") == 0)
            {
               cJSON *delta = cJSON_GetObjectItem(event, "delta");
               cJSON *dt = delta ? cJSON_GetObjectItem(delta, "type") : NULL;
               const char *dts = (dt && cJSON_IsString(dt)) ? dt->valuestring : "";

               if (strcmp(dts, "text_delta") == 0)
               {
                  cJSON *text = cJSON_GetObjectItem(delta, "text");
                  if (text && cJSON_IsString(text) && text->valuestring[0])
                     stream_event(cctx, "text", "content", text->valuestring);
               }
            }
            else if (strcmp(et, "message_stop") == 0)
            {
               stream_event(cctx, "turn_end", NULL, NULL);
            }
         }
      }
      else if (strcmp(type, "tool_result") == 0 || strcmp(type, "user") == 0)
      {
         had_tool_result = 1;
      }
      else if (strcmp(type, "result") == 0)
      {
         cJSON *sid = cJSON_GetObjectItem(obj, "session_id");
         if (sid && cJSON_IsString(sid))
            stream_event(cctx, "session", "id", sid->valuestring);
      }

      cJSON_Delete(obj);
   }

   free(line);
   fclose(fp);
   waitpid(pid, NULL, 0);
   stream_event(cctx, "done", NULL, NULL);
   compute_ctx_free(cctx);
}

int handle_chat_send_stream(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   compute_ctx_t *cctx = create_compute_ctx(ctx, conn, req);
   if (!cctx)
      return server_send_error(conn, "out of memory", NULL);

   if (compute_pool_submit(&ctx->pool, chat_stream_worker, cctx) != 0)
   {
      compute_ctx_free(cctx);
      return server_send_error(conn, "compute queue full", NULL);
   }

   return 0;
}
