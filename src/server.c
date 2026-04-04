/* server.c: aimee-server core -- event loop, connection handling, method dispatch */
#define _GNU_SOURCE
#include "aimee.h"
#include "server.h"
#include "log.h"
#include "platform_event.h"
#include "platform_ipc.h"
#include "cJSON.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* --- Connection helpers --- */

static server_conn_t *conn_find(server_ctx_t *ctx, int fd)
{
   for (int i = 0; i < ctx->conn_count; i++)
   {
      if (ctx->conns[i].fd == fd)
         return &ctx->conns[i];
   }
   return NULL;
}

static void conn_close(server_ctx_t *ctx, server_conn_t *conn)
{
   if (!conn || conn->fd < 0)
      return;

   platform_evloop_del(&ctx->evloop, conn->fd);
   close(conn->fd);

   if (conn->db)
   {
      sqlite3_close(conn->db);
      conn->db = NULL;
   }

   /* Swap with last element to keep array compact */
   int idx = (int)(conn - ctx->conns);
   if (idx < ctx->conn_count - 1)
      ctx->conns[idx] = ctx->conns[ctx->conn_count - 1];
   ctx->conn_count--;
}

static int64_t monotonic_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Update event loop registration: IN always, OUT when there's pending data */
static void conn_update_events(server_conn_t *conn)
{
   uint32_t events = PLAT_EV_IN;
   if (conn->write_len > 0)
      events |= PLAT_EV_OUT;
   platform_evloop_mod(conn->evloop, conn->fd, events);
}

/* Flush pending output buffer to kernel. Returns 0 on success/partial, -1 on error. */
static int conn_flush(server_conn_t *conn)
{
   while (conn->write_len > 0)
   {
      ssize_t n = write(conn->fd, conn->write_buf + conn->write_pos, conn->write_len);
      if (n < 0)
      {
         if (errno == EINTR)
            continue;
         if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0; /* can't write more right now */
         return -1;
      }
      conn->write_pos += (size_t)n;
      conn->write_len -= (size_t)n;
   }

   /* All flushed: reset buffer and clear deadline */
   conn->write_pos = 0;
   conn->write_len = 0;
   conn->write_deadline_ms = 0;
   conn_update_events(conn);
   return 0;
}

/* Queue data for sending. Tries to write immediately; buffers remainder. */
static int conn_send(server_conn_t *conn, const char *data, size_t len)
{
   /* Try to flush any existing pending data first */
   if (conn->write_len > 0)
   {
      if (conn_flush(conn) < 0)
         return -1;
   }

   /* If buffer is empty, try writing directly to avoid buffering */
   if (conn->write_len == 0)
   {
      while (len > 0)
      {
         ssize_t n = write(conn->fd, data, len);
         if (n < 0)
         {
            if (errno == EINTR)
               continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
               break; /* buffer the rest */
            return -1;
         }
         data += n;
         len -= (size_t)n;
      }

      if (len == 0)
         return 0; /* all sent immediately */
   }

   /* Buffer remaining data */
   /* Compact buffer if needed to make room at the end */
   if (conn->write_pos > 0 && conn->write_pos + conn->write_len + len > SERVER_WRITE_BUF_SIZE)
   {
      memmove(conn->write_buf, conn->write_buf + conn->write_pos, conn->write_len);
      conn->write_pos = 0;
   }

   /* Check if there's enough room */
   if (conn->write_pos + conn->write_len + len > SERVER_WRITE_BUF_SIZE)
      return -1; /* buffer overflow: client too slow */

   memcpy(conn->write_buf + conn->write_pos + conn->write_len, data, len);
   conn->write_len += len;

   /* Set write deadline if this is the first pending data */
   if (conn->write_deadline_ms == 0)
      conn->write_deadline_ms = monotonic_ms() + CONN_WRITE_DEADLINE_MS;

   conn_update_events(conn);
   return 0;
}

int server_send_response(server_conn_t *conn, cJSON *resp)
{
   char *json_str = cJSON_PrintUnformatted(resp);
   if (!json_str)
      return -1;

   size_t len = strlen(json_str);
   int rc = conn_send(conn, json_str, len);
   free(json_str);
   if (rc < 0)
      return -1;

   /* Send newline delimiter */
   return conn_send(conn, "\n", 1);
}

int server_send_error(server_conn_t *conn, const char *message, const char *request_id)
{
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "error");
   cJSON_AddStringToObject(resp, "message", message);
   if (request_id)
      cJSON_AddStringToObject(resp, "request_id", request_id);
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

/* --- Method handlers --- */

static int handle_server_info(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddNumberToObject(resp, "protocol_version", SERVER_PROTOCOL_VERSION);
   cJSON_AddStringToObject(resp, "server_version", AIMEE_VERSION);
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

static int handle_server_health(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)req;

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddNumberToObject(resp, "uptime", (double)(time(NULL) - ctx->start_time));
   cJSON_AddStringToObject(resp, "db", ctx->db ? "ok" : "unavailable");
   cJSON_AddNumberToObject(resp, "connections", ctx->conn_count);
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

static int handle_hooks_pre(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   cJSON *jtn = cJSON_GetObjectItemCaseSensitive(req, "tool_name");
   cJSON *jti = cJSON_GetObjectItemCaseSensitive(req, "tool_input");
   cJSON *jcwd = cJSON_GetObjectItemCaseSensitive(req, "cwd");
   cJSON *jsid = cJSON_GetObjectItemCaseSensitive(req, "session_id");
   cJSON *jrid = cJSON_GetObjectItemCaseSensitive(req, "request_id");

   const char *request_id = cJSON_IsString(jrid) ? jrid->valuestring : NULL;

   if (!cJSON_IsString(jtn))
      return server_send_error(conn, "missing tool_name", request_id);

   const char *tool_name = jtn->valuestring;
   char *ti_heap = NULL;
   const char *tool_input = "{}";

   if (cJSON_IsString(jti))
      tool_input = jti->valuestring;
   else if (cJSON_IsObject(jti) || cJSON_IsArray(jti))
   {
      ti_heap = cJSON_PrintUnformatted(jti);
      tool_input = ti_heap;
   }

   const char *cwd = cJSON_IsString(jcwd) ? jcwd->valuestring : "";
   const char *sid = cJSON_IsString(jsid) ? jsid->valuestring : "unknown";
   if (!is_safe_id(sid))
      return server_send_error(conn, "invalid session_id (must be alphanumeric/dash/underscore)",
                               request_id);

   /* Load config */
   config_t cfg;
   config_load(&cfg);

   /* Load session state using provided session_id */
   char state_path[MAX_PATH_LEN];
   snprintf(state_path, sizeof(state_path), "%s/session-%s.state", config_output_dir(), sid);
   session_state_t state;
   session_state_load(&state, state_path);

   /* Run guardrail check */
   char msg[1024] = "";
   int rc = pre_tool_check(conn->db, tool_name, tool_input, &state, config_guardrail_mode(&cfg),
                           cwd, msg, sizeof(msg));

   session_state_save(&state, state_path);

   /* Build response */
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", rc == 0 ? "ok" : "blocked");
   cJSON_AddNumberToObject(resp, "exit_code", rc);
   if (msg[0])
      cJSON_AddStringToObject(resp, "message", msg);
   if (request_id)
      cJSON_AddStringToObject(resp, "request_id", request_id);

   int src = server_send_response(conn, resp);
   cJSON_Delete(resp);
   if (ti_heap)
      free(ti_heap);
   return src;
}

static int handle_hooks_post(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   cJSON *jtn = cJSON_GetObjectItemCaseSensitive(req, "tool_name");
   cJSON *jti = cJSON_GetObjectItemCaseSensitive(req, "tool_input");
   cJSON *jrid = cJSON_GetObjectItemCaseSensitive(req, "request_id");
   const char *request_id = cJSON_IsString(jrid) ? jrid->valuestring : NULL;

   const char *tool_name = cJSON_IsString(jtn) ? jtn->valuestring : "";
   char *ti_heap = NULL;
   const char *tool_input = "{}";

   if (cJSON_IsString(jti))
      tool_input = jti->valuestring;
   else if (cJSON_IsObject(jti) || cJSON_IsArray(jti))
   {
      ti_heap = cJSON_PrintUnformatted(jti);
      tool_input = ti_heap;
   }

   post_tool_update(conn->db, tool_name, tool_input);

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   if (request_id)
      cJSON_AddStringToObject(resp, "request_id", request_id);
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   if (ti_heap)
      free(ti_heap);
   return rc;
}

/* --- Dispatch --- */

/* Check JSON nesting depth (recursive) */
static int json_check_depth(const cJSON *item, int depth, int max_depth)
{
   if (depth > max_depth)
      return -1;
   if (!item)
      return 0;

   /* Check field count for objects */
   if (cJSON_IsObject(item))
   {
      int count = 0;
      const cJSON *child = item->child;
      while (child)
      {
         count++;
         if (count > JSON_MAX_FIELDS)
            return -2;
         if (json_check_depth(child, depth + 1, max_depth) != 0)
            return -1;
         child = child->next;
      }
   }
   else if (cJSON_IsArray(item))
   {
      const cJSON *child = item->child;
      while (child)
      {
         if (json_check_depth(child, depth + 1, max_depth) != 0)
            return -1;
         child = child->next;
      }
   }
   return 0;
}

/* Get per-method size limit based on method prefix */
static size_t method_size_limit(const char *method)
{
   if (!method)
      return LIMIT_DEFAULT;

   static const struct
   {
      const char *prefix;
      size_t max;
   } limits[] = {
       {"memory.", LIMIT_MEMORY}, {"tool.", LIMIT_TOOL}, {"delegate", LIMIT_DELEGATE},
       {"chat.", LIMIT_CHAT},     {NULL, LIMIT_DEFAULT},
   };

   for (int i = 0; limits[i].prefix; i++)
   {
      if (strncmp(method, limits[i].prefix, strlen(limits[i].prefix)) == 0)
         return limits[i].max;
   }
   return LIMIT_DEFAULT;
}

int server_dispatch(server_ctx_t *ctx, server_conn_t *conn, const char *msg, size_t msg_len)
{
   /* Quick method extraction for size limit check (scan for "method":"..." in raw JSON) */
   const char *method_start = strstr(msg, "\"method\"");
   char quick_method[64] = "";
   if (method_start)
   {
      const char *val = strchr(method_start + 8, '"');
      if (val)
      {
         val++; /* skip opening quote */
         const char *end = strchr(val, '"');
         if (end && (size_t)(end - val) < sizeof(quick_method))
         {
            memcpy(quick_method, val, (size_t)(end - val));
            quick_method[end - val] = '\0';
         }
      }
   }

   /* Check per-method size limit before parsing */
   size_t limit = method_size_limit(quick_method);
   if (msg_len > limit)
   {
      char errmsg[256];
      snprintf(errmsg, sizeof(errmsg),
               "PAYLOAD_TOO_LARGE: %zu bytes exceeds %zu limit for method '%s'", msg_len, limit,
               quick_method);
      return server_send_error(conn, errmsg, NULL);
   }

   cJSON *req = cJSON_Parse(msg);
   if (!req)
      return server_send_error(conn, "invalid JSON", NULL);

   /* Check JSON depth and field count */
   if (json_check_depth(req, 0, JSON_MAX_DEPTH) != 0)
   {
      cJSON_Delete(req);
      return server_send_error(conn, "PAYLOAD_MALFORMED: JSON exceeds depth/field limits", NULL);
   }

   cJSON *method = cJSON_GetObjectItemCaseSensitive(req, "method");
   if (!cJSON_IsString(method))
   {
      cJSON_Delete(req);
      return server_send_error(conn, "missing method", NULL);
   }

   const char *m = method->valuestring;
   int rc;

   /* Capability check */
   uint32_t required = server_capability_for_method(m);
   if (required && (conn->capabilities & required) == 0)
   {
      const method_policy_t *policy = server_policy_for_method(m);

      cJSON *resp = cJSON_CreateObject();
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "code", "AUTHZ_DENIED");
      cJSON_AddStringToObject(resp, "message", "forbidden: insufficient capabilities");
      cJSON_AddStringToObject(resp, "method", m);
      if (policy)
         cJSON_AddStringToObject(resp, "description", policy->description);

      /* Principal identifier */
      char principal[64];
      snprintf(principal, sizeof(principal), "uid:%u", (unsigned)conn->peer_uid);
      cJSON_AddStringToObject(resp, "principal", principal);

      cJSON_AddNumberToObject(resp, "required_caps", required);
      cJSON_AddNumberToObject(resp, "held_caps", conn->capabilities);

      /* Log for operator forensics */
      fprintf(stderr,
              "aimee-server: AUTHZ_DENIED method=%s principal=%s "
              "required=0x%04x held=0x%04x\n",
              m, principal, required, conn->capabilities);

      int src = server_send_response(conn, resp);
      cJSON_Delete(resp);
      cJSON_Delete(req);
      return src;
   }

   /* Method dispatch table — add new methods here instead of extending an if-else chain */
   typedef int (*method_handler_t)(server_ctx_t *, server_conn_t *, cJSON *);
   static const struct
   {
      const char *method;
      method_handler_t handler;
   } dispatch_table[] = {
       /* Server */
       {"server.info", handle_server_info},
       {"server.health", handle_server_health},
       {"auth", handle_auth},
       /* Hooks */
       {"hooks.pre", handle_hooks_pre},
       {"hooks.post", handle_hooks_post},
       /* Sessions */
       {"session.create", handle_session_create},
       {"session.list", handle_session_list},
       {"session.get", handle_session_get},
       {"session.close", handle_session_close},
       /* Memory */
       {"memory.search", handle_memory_search},
       {"memory.store", handle_memory_store},
       {"memory.list", handle_memory_list},
       {"memory.get", handle_memory_get},
       /* Index */
       {"index.find", handle_index_find},
       {"index.blast_radius", handle_index_blast_radius},
       {"blast_radius.preview", handle_blast_radius_preview},
       {"index.list", handle_index_list},
       /* Rules */
       {"rules.list", handle_rules_list},
       {"rules.generate", handle_rules_generate},
       /* Working memory */
       {"wm.set", handle_wm_set},
       {"wm.get", handle_wm_get},
       {"wm.list", handle_wm_list},
       {"wm.context", handle_wm_context},
       /* Attempt log */
       {"attempt.record", handle_attempt_record},
       {"attempt.list", handle_attempt_list},
       /* Dashboard */
       {"dashboard.metrics", handle_dashboard_metrics},
       {"dashboard.delegations", handle_dashboard_delegations},
       /* Workspace */
       {"workspace.context", handle_workspace_context},
       /* Compute (thread pool) */
       {"tool.execute", handle_tool_execute},
       {"delegate", handle_delegate},
       {"delegate.reply", handle_delegate_reply},
       {"chat.send_stream", handle_chat_send_stream},
       /* MCP proxy */
       {"mcp.call", handle_mcp_call},
       /* CLI forwarding */
       {"cli.forward", handle_cli_forward},
       {NULL, NULL},
   };

   rc = -1;
   for (int i = 0; dispatch_table[i].method; i++)
   {
      if (strcmp(m, dispatch_table[i].method) == 0)
      {
         rc = dispatch_table[i].handler(ctx, conn, req);
         break;
      }
   }
   if (rc == -1)
      rc = server_send_error(conn, "unknown method", NULL);

   cJSON_Delete(req);
   return rc;
}

/* --- Accept --- */

static int accept_connection(server_ctx_t *ctx)
{
   int fd = platform_ipc_accept(ctx->listen_fd);
   if (fd < 0)
      return -1;

   if (ctx->conn_count >= SERVER_MAX_CONNECTIONS)
   {
      close(fd);
      return -1;
   }

   /* Get peer credentials */
   platform_peer_cred_t cred;
   if (platform_ipc_peer_cred(fd, &cred) < 0)
   {
      close(fd);
      return -1;
   }

   /* Open per-connection DB handle */
   config_t cfg;
   config_load(&cfg);

   sqlite3 *db = db_open_fast(cfg.db_path);

   /* Initialize connection */
   server_conn_t *conn = &ctx->conns[ctx->conn_count];
   memset(conn, 0, sizeof(*conn));
   conn->fd = fd;
   conn->evloop = &ctx->evloop;
   conn->peer_uid = cred.uid;
   conn->peer_gid = cred.gid;
   conn->peer_pid = cred.pid;
   conn->db = db;
   /* Start with read-only capabilities for same-UID connections */
   conn->capabilities = (cred.uid == getuid()) ? CAPS_READ_ONLY : 0;

   ctx->conn_count++;

   /* Add to event loop */
   if (platform_evloop_add(&ctx->evloop, fd, PLAT_EV_IN) < 0)
   {
      conn_close(ctx, conn);
      return -1;
   }

   return 0;
}

/* --- Read and process --- */

static void process_connection(server_ctx_t *ctx, server_conn_t *conn)
{
   /* Read available data */
   size_t space = SERVER_READ_BUF_SIZE - 1 - conn->read_len;
   if (space == 0)
   {
      /* Buffer full, no newline found -- drop connection */
      conn_close(ctx, conn);
      return;
   }

   ssize_t n = read(conn->fd, conn->read_buf + conn->read_len, space);
   if (n <= 0)
   {
      if (n == 0 || (errno != EAGAIN && errno != EINTR))
         conn_close(ctx, conn);
      return;
   }
   conn->read_len += (size_t)n;

   /* Process complete messages (newline-delimited) */
   for (;;)
   {
      char *nl = memchr(conn->read_buf, '\n', conn->read_len);
      if (!nl)
         break;

      size_t msg_len = (size_t)(nl - conn->read_buf);
      conn->read_buf[msg_len] = '\0';

      server_dispatch(ctx, conn, conn->read_buf, msg_len);

      /* Shift remaining data */
      size_t remain = conn->read_len - msg_len - 1;
      if (remain > 0)
         memmove(conn->read_buf, nl + 1, remain);
      conn->read_len = remain;
   }
}

/* --- Lifecycle --- */

int server_init(server_ctx_t *ctx, const char *socket_path)
{
   memset(ctx, 0, sizeof(*ctx));
   ctx->listen_fd = -1;
   ctx->start_time = time(NULL);
   snprintf(ctx->socket_path, sizeof(ctx->socket_path), "%s", socket_path);

   /* Check for stale socket and handle symlink rejection */
   struct stat st;
   if (lstat(socket_path, &st) == 0)
   {
      if (S_ISLNK(st.st_mode))
      {
         LOG_ERROR("server", "socket path is a symlink, refusing");
         return -1;
      }

      if (S_ISSOCK(st.st_mode))
      {
         if (platform_ipc_probe(socket_path) == 0)
         {
            LOG_ERROR("server", "another server is already running");
            return -1;
         }
         /* Stale socket, remove it */
         unlink(socket_path);
      }
   }

   /* Create listening IPC socket */
   int fd = platform_ipc_listen(socket_path, SERVER_LISTEN_BACKLOG);
   if (fd < 0)
   {
      perror("aimee-server: ipc listen");
      return -1;
   }

   /* Create event loop */
   if (platform_evloop_create(&ctx->evloop) < 0)
   {
      perror("aimee-server: evloop create");
      close(fd);
      unlink(socket_path);
      return -1;
   }

   if (platform_evloop_add(&ctx->evloop, fd, PLAT_EV_IN) < 0)
   {
      perror("aimee-server: evloop add listen fd");
      platform_evloop_destroy(&ctx->evloop);
      close(fd);
      unlink(socket_path);
      return -1;
   }

   ctx->listen_fd = fd;

   /* Open main-thread DB (full open to run migrations) */
   config_t cfg;
   config_load(&cfg);
   ctx->db = db_open(cfg.db_path);

   /* Apply server-mode pragma profile (overrides CLI defaults set by db_open) */
   if (ctx->db)
      db_apply_pragmas(ctx->db, DB_MODE_SERVER);

   /* Load or generate capability token */
   if (server_load_token(ctx) != 0)
      LOG_WARN("server", "could not load/generate auth token");

   /* Initialize compute thread pool */
   if (compute_pool_init(&ctx->pool, COMPUTE_POOL_SIZE) != 0)
   {
      LOG_ERROR("server", "failed to initialize compute pool");
      close(fd);
      platform_evloop_destroy(&ctx->evloop);
      unlink(socket_path);
      return -1;
   }

   LOG_INFO("server", "listening on %s (v%s, protocol %d, %d workers)", socket_path, AIMEE_VERSION,
            SERVER_PROTOCOL_VERSION, COMPUTE_POOL_SIZE);

   return 0;
}

int server_run(server_ctx_t *ctx)
{
   platform_event_t events[64];
   time_t last_idle_check = time(NULL);

   while (ctx->running)
   {
      int n = platform_evloop_wait(&ctx->evloop, events, 64, 1000);
      if (n < 0)
      {
         perror("aimee-server: event wait");
         return -1;
      }

      for (int i = 0; i < n; i++)
      {
         if (events[i].fd == ctx->listen_fd)
         {
            /* Accept new connections (drain accept queue) */
            while (accept_connection(ctx) == 0)
               ;
         }
         else
         {
            server_conn_t *conn = conn_find(ctx, events[i].fd);
            if (!conn)
               continue;

            if (events[i].events & (PLAT_EV_HUP | PLAT_EV_ERR))
            {
               conn_close(ctx, conn);
               continue;
            }

            /* Flush pending output when socket becomes writable */
            if (events[i].events & PLAT_EV_OUT)
            {
               if (conn_flush(conn) < 0)
               {
                  conn_close(ctx, conn);
                  continue;
               }
            }

            if (events[i].events & PLAT_EV_IN)
               process_connection(ctx, conn);
         }
      }

      /* Check write deadlines on every iteration (cheap monotonic comparison) */
      {
         int64_t now_ms = monotonic_ms();
         for (int ci = ctx->conn_count - 1; ci >= 0; ci--)
         {
            server_conn_t *c = &ctx->conns[ci];
            if (c->write_deadline_ms > 0 && now_ms >= c->write_deadline_ms)
            {
               LOG_WARN("server", "closing connection fd=%d: write deadline exceeded", c->fd);
               conn_close(ctx, c);
            }
         }
      }

      /* Session-aware idle timeout (non-persistent servers only, every ~5s) */
      if (!ctx->persistent)
      {
         time_t now = time(NULL);
         if (now - last_idle_check >= 5)
         {
            last_idle_check = now;

            if (ctx->last_session_end > 0 && now - ctx->last_session_end > SERVER_IDLE_TIMEOUT)
            {
               LOG_INFO("server", "idle timeout (%ds since last session), shutting down",
                        (int)(now - ctx->last_session_end));
               ctx->running = 0;
            }
         }
      }
   }

   return 0;
}

void server_shutdown(server_ctx_t *ctx)
{
   /* Shut down compute pool first (drain in-flight work) */
   compute_pool_shutdown(&ctx->pool);

   /* Close all client connections */
   while (ctx->conn_count > 0)
      conn_close(ctx, &ctx->conns[0]);

   /* Close listen socket */
   if (ctx->listen_fd >= 0)
   {
      platform_evloop_del(&ctx->evloop, ctx->listen_fd);
      close(ctx->listen_fd);
      ctx->listen_fd = -1;
   }

   /* Clean up event loop */
   platform_evloop_destroy(&ctx->evloop);

   /* Unlink socket */
   if (ctx->socket_path[0])
      unlink(ctx->socket_path);

   /* Close main DB */
   if (ctx->db)
   {
      sqlite3_close(ctx->db);
      ctx->db = NULL;
   }

   LOG_INFO("server", "shut down");
}
