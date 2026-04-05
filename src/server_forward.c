/* server_forward.c: cli.forward handler -- dispatches any aimee command in a forked child */
#define _GNU_SOURCE
#include "aimee.h"
#include "commands.h"
#include "server.h"
#include "compute_pool.h"
#include "platform_random.h"
#include "cJSON.h"
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Compute context (same as server_compute.c) */
typedef struct
{
   server_ctx_t *server;
   int conn_fd;
   sqlite3 *db;
   pthread_mutex_t *write_mutex;
   cJSON *req;
} forward_ctx_t;

static int write_all_fwd(int fd, const char *data, size_t len)
{
   size_t total = 0;
   while (total < len)
   {
      ssize_t n = write(fd, data + total, len - total);
      if (n > 0)
         total += (size_t)n;
      else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      {
         struct pollfd pfd = {.fd = fd, .events = POLLOUT};
         if (poll(&pfd, 1, 30000) <= 0)
            return -1;
      }
      else
         return -1;
   }
   return 0;
}

static void forward_respond(forward_ctx_t *fctx, cJSON *resp)
{
   char *json_str = cJSON_PrintUnformatted(resp);
   if (json_str)
   {
      pthread_mutex_lock(fctx->write_mutex);
      write_all_fwd(fctx->conn_fd, json_str, strlen(json_str));
      write_all_fwd(fctx->conn_fd, "\n", 1);
      pthread_mutex_unlock(fctx->write_mutex);
      free(json_str);
   }
   cJSON_Delete(resp);
}

static void forward_ctx_free(forward_ctx_t *fctx)
{
   if (fctx->req)
      cJSON_Delete(fctx->req);
   if (fctx->db)
      sqlite3_close(fctx->db);
   if (fctx->write_mutex)
   {
      pthread_mutex_destroy(fctx->write_mutex);
      free(fctx->write_mutex);
   }
   free(fctx);
}

static void forward_worker(void *arg)
{
   forward_ctx_t *fctx = (forward_ctx_t *)arg;
   cJSON *req = fctx->req;

   cJSON *jcmd = cJSON_GetObjectItemCaseSensitive(req, "command");
   cJSON *jargs = cJSON_GetObjectItemCaseSensitive(req, "args");
   cJSON *jjson = cJSON_GetObjectItemCaseSensitive(req, "json");
   cJSON *jcwd = cJSON_GetObjectItemCaseSensitive(req, "cwd");
   cJSON *jstdin = cJSON_GetObjectItemCaseSensitive(req, "stdin");

   const char *cmd_name = cJSON_IsString(jcmd) ? jcmd->valuestring : "";
   int want_json = cJSON_IsTrue(jjson);
   const char *cwd = cJSON_IsString(jcwd) ? jcwd->valuestring : NULL;
   const char *stdin_data = cJSON_IsString(jstdin) ? jstdin->valuestring : NULL;

   if (!cmd_name[0])
   {
      cJSON *resp = cJSON_CreateObject();
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "missing command");
      forward_respond(fctx, resp);
      forward_ctx_free(fctx);
      return;
   }

   /* Build argv from JSON args array */
   int argc = 0;
   char *argv[64];
   if (cJSON_IsArray(jargs))
   {
      int count = cJSON_GetArraySize(jargs);
      if (count > 62)
         count = 62;
      for (int i = 0; i < count; i++)
      {
         cJSON *item = cJSON_GetArrayItem(jargs, i);
         if (cJSON_IsString(item))
            argv[argc++] = item->valuestring;
      }
   }
   argv[argc] = NULL;

   /* Create pipe to capture stdout */
   int out_pipe[2];
   int in_pipe[2] = {-1, -1};
   if (pipe(out_pipe) < 0)
   {
      cJSON *resp = cJSON_CreateObject();
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "pipe failed");
      forward_respond(fctx, resp);
      forward_ctx_free(fctx);
      return;
   }

   if (stdin_data && pipe(in_pipe) < 0)
   {
      close(out_pipe[0]);
      close(out_pipe[1]);
      cJSON *resp = cJSON_CreateObject();
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "pipe failed");
      forward_respond(fctx, resp);
      forward_ctx_free(fctx);
      return;
   }

   pid_t pid = fork();
   if (pid < 0)
   {
      close(out_pipe[0]);
      close(out_pipe[1]);
      if (in_pipe[0] >= 0)
      {
         close(in_pipe[0]);
         close(in_pipe[1]);
      }
      cJSON *resp = cJSON_CreateObject();
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "fork failed");
      forward_respond(fctx, resp);
      forward_ctx_free(fctx);
      return;
   }

   if (pid == 0)
   {
      /* Child process -- dispatch command in-process */
      close(out_pipe[0]);
      dup2(out_pipe[1], STDOUT_FILENO);
      dup2(out_pipe[1], STDERR_FILENO);
      close(out_pipe[1]);

      if (in_pipe[0] >= 0)
      {
         close(in_pipe[1]);
         dup2(in_pipe[0], STDIN_FILENO);
         close(in_pipe[0]);
      }

      if (cwd)
      {
         /* Validate cwd: must be absolute, no traversal, must exist */
         if (cwd[0] == '/' && !strstr(cwd, "/../") && !strstr(cwd, "/.."))
            (void)chdir(cwd);
      }

      /* Set CLAUDE_SESSION_ID for the child process.
       * session-start/launch: generate a fresh UUID so each session gets a
       * unique identity instead of reusing the most recent DB entry.
       * Other commands: use client-provided session_id, or fall back to DB. */
      {
         int is_session_init =
             (strcmp(cmd_name, "launch") == 0 || strcmp(cmd_name, "session-start") == 0);

         if (is_session_init)
         {
            unsigned char raw[16];
            if (platform_random_bytes(raw, sizeof(raw)) != 0)
               memset(raw, 0, sizeof(raw));
            char new_sid[64];
            snprintf(new_sid, sizeof(new_sid),
                     "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", raw[0],
                     raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7], raw[8], raw[9],
                     raw[10], raw[11], raw[12], raw[13], raw[14], raw[15]);
            setenv("CLAUDE_SESSION_ID", new_sid, 1);

            sqlite3 *sid_db = fctx->server->db;
            if (sid_db)
            {
               char ts[32];
               now_utc(ts, sizeof(ts));
               char principal[32];
               snprintf(principal, sizeof(principal), "uid:%d", (int)getuid());
               const char *ins_sql =
                   "INSERT INTO server_sessions (id, client_type, principal, title, "
                   "created_at, last_activity_at) VALUES (?, 'cli', ?, '', ?, ?)";
               sqlite3_stmt *ins_stmt = NULL;
               if (sqlite3_prepare_v2(sid_db, ins_sql, -1, &ins_stmt, NULL) == SQLITE_OK)
               {
                  sqlite3_bind_text(ins_stmt, 1, new_sid, -1, SQLITE_TRANSIENT);
                  sqlite3_bind_text(ins_stmt, 2, principal, -1, SQLITE_TRANSIENT);
                  sqlite3_bind_text(ins_stmt, 3, ts, -1, SQLITE_TRANSIENT);
                  sqlite3_bind_text(ins_stmt, 4, ts, -1, SQLITE_TRANSIENT);
                  DB_STEP_LOG(ins_stmt, "server_forward");
                  sqlite3_finalize(ins_stmt);
               }
            }
         }
         else
         {
            cJSON *jsid = cJSON_GetObjectItemCaseSensitive(req, "session_id");
            if (cJSON_IsString(jsid) && jsid->valuestring[0])
            {
               setenv("CLAUDE_SESSION_ID", jsid->valuestring, 1);
            }
            else
            {
               sqlite3 *sid_db = fctx->server->db;
               if (sid_db)
               {
                  const char *sid_sql = "SELECT id FROM server_sessions "
                                        "WHERE created_at > datetime('now', '-7 days') "
                                        "ORDER BY created_at DESC LIMIT 1";
                  sqlite3_stmt *sid_stmt = NULL;
                  if (sqlite3_prepare_v2(sid_db, sid_sql, -1, &sid_stmt, NULL) == SQLITE_OK)
                  {
                     if (sqlite3_step(sid_stmt) == SQLITE_ROW)
                     {
                        const char *sid = (const char *)sqlite3_column_text(sid_stmt, 0);
                        if (sid && sid[0])
                           setenv("CLAUDE_SESSION_ID", sid, 1);
                     }
                     sqlite3_finalize(sid_stmt);
                  }
               }
            }
         }
      }

      /* Dispatch command in-process via the command table */
      {
         app_ctx_t child_ctx;
         memset(&child_ctx, 0, sizeof(child_ctx));
         child_ctx.json_output = want_json;

         /* Open a fresh DB connection for the child */
         child_ctx.db = db_open(NULL);

         /* Look up and execute the command handler */
         int found = 0;
         for (int ci = 0; commands[ci].name != NULL; ci++)
         {
            if (strcmp(commands[ci].name, cmd_name) == 0)
            {
               commands[ci].handler(&child_ctx, argc, argv);
               found = 1;
               break;
            }
         }
         if (!found)
            fprintf(stderr, "aimee: unknown command '%s'\n", cmd_name);

         if (child_ctx.db)
         {
            db_stmt_cache_clear();
            sqlite3_close(child_ctx.db);
         }
         fflush(stdout);
         fflush(stderr);
         _exit(found ? 0 : 1);
      }
   }

   /* Parent */
   close(out_pipe[1]);
   if (in_pipe[0] >= 0)
      close(in_pipe[0]);

   /* Write stdin data if provided */
   if (stdin_data && in_pipe[1] >= 0)
   {
      size_t slen = strlen(stdin_data);
      size_t written = 0;
      while (written < slen)
      {
         ssize_t w = write(in_pipe[1], stdin_data + written, slen - written);
         if (w <= 0)
            break;
         written += (size_t)w;
      }
      close(in_pipe[1]);
   }

   /* Stream output from child to client in real-time.
    * Each chunk is sent as {"event":"output","data":"..."}\n so the client
    * can display it immediately (important for interactive flows like OAuth). */
   int child_status = 0;
   int child_reaped = 0;
   char chunk[4096];
   for (;;)
   {
      struct pollfd pfd = {.fd = out_pipe[0], .events = POLLIN};
      int pr = poll(&pfd, 1, 1000); /* 1s poll to check child liveness */
      if (pr > 0 && (pfd.revents & POLLIN))
      {
         ssize_t n = read(out_pipe[0], chunk, sizeof(chunk) - 1);
         if (n <= 0)
            break;
         chunk[n] = '\0';

         cJSON *evt = cJSON_CreateObject();
         cJSON_AddStringToObject(evt, "event", "output");
         cJSON_AddStringToObject(evt, "data", chunk);
         char *json_str = cJSON_PrintUnformatted(evt);
         cJSON_Delete(evt);
         if (json_str)
         {
            pthread_mutex_lock(fctx->write_mutex);
            write_all_fwd(fctx->conn_fd, json_str, strlen(json_str));
            write_all_fwd(fctx->conn_fd, "\n", 1);
            pthread_mutex_unlock(fctx->write_mutex);
            free(json_str);
         }
      }
      else if (pr == 0)
      {
         /* Poll timeout — check if child is still alive */
         pid_t w = waitpid(pid, &child_status, WNOHANG);
         if (w > 0)
         {
            child_reaped = 1;
            /* Child exited; drain any remaining output */
            for (;;)
            {
               ssize_t n = read(out_pipe[0], chunk, sizeof(chunk) - 1);
               if (n <= 0)
                  break;
               chunk[n] = '\0';

               cJSON *evt2 = cJSON_CreateObject();
               cJSON_AddStringToObject(evt2, "event", "output");
               cJSON_AddStringToObject(evt2, "data", chunk);
               char *js2 = cJSON_PrintUnformatted(evt2);
               cJSON_Delete(evt2);
               if (js2)
               {
                  pthread_mutex_lock(fctx->write_mutex);
                  write_all_fwd(fctx->conn_fd, js2, strlen(js2));
                  write_all_fwd(fctx->conn_fd, "\n", 1);
                  pthread_mutex_unlock(fctx->write_mutex);
                  free(js2);
               }
            }
            break;
         }
      }
      else
      {
         /* poll error */
         break;
      }
   }
   close(out_pipe[0]);

   if (!child_reaped)
      waitpid(pid, &child_status, 0);
   int exit_code = WIFEXITED(child_status) ? WEXITSTATUS(child_status) : 1;

   /* Session lifecycle tracking */
   if (strcmp(cmd_name, "launch") == 0 || strcmp(cmd_name, "session-start") == 0)
   {
      __atomic_add_fetch(&fctx->server->active_sessions, 1, __ATOMIC_SEQ_CST);
      fctx->server->last_session_end = 0;
   }
   else if (strcmp(cmd_name, "wrapup") == 0)
   {
      int prev = __atomic_sub_fetch(&fctx->server->active_sessions, 1, __ATOMIC_SEQ_CST);
      if (prev <= 0)
      {
         __atomic_store_n(&fctx->server->active_sessions, 0, __ATOMIC_SEQ_CST);
         fctx->server->last_session_end = time(NULL);
         if (!fctx->server->persistent)
         {
            fprintf(stderr, "aimee-server: last session ended, shutting down\n");
            fctx->server->running = 0;
         }
      }
   }

   /* Build final response (output was already streamed) */
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", exit_code == 0 ? "ok" : "error");
   cJSON_AddStringToObject(resp, "output", "");
   cJSON_AddNumberToObject(resp, "exit_code", exit_code);

   forward_respond(fctx, resp);
   forward_ctx_free(fctx);
}

int handle_cli_forward(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   forward_ctx_t *fctx = calloc(1, sizeof(forward_ctx_t));
   if (!fctx)
      return server_send_error(conn, "out of memory", NULL);

   fctx->server = ctx;
   fctx->conn_fd = conn->fd;
   fctx->req = cJSON_Duplicate(req, 1);

   config_t cfg;
   config_load(&cfg);
   fctx->db = db_open_fast(cfg.db_path);

   fctx->write_mutex = malloc(sizeof(pthread_mutex_t));
   pthread_mutex_init(fctx->write_mutex, NULL);

   if (compute_pool_submit(&ctx->pool, forward_worker, fctx) != 0)
   {
      forward_ctx_free(fctx);
      return server_send_error(conn, "compute queue full", NULL);
   }

   return 0;
}
