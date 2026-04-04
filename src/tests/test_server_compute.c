#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include "aimee.h"
#include "server.h"

#include "../server_compute.c"

static void *waiter_thread(void *arg)
{
   delegation_mailbox_t *mb = (delegation_mailbox_t *)arg;
   char *out = malloc(64);
   assert(out != NULL);
   int rc = mailbox_wait(mb, out, 64, 2);
   assert(rc == 0);
   return out;
}

typedef struct
{
   int fd;
   size_t total;
} drain_ctx_t;

static void *drain_pipe_thread(void *arg)
{
   drain_ctx_t *ctx = (drain_ctx_t *)arg;
   char buf[4096];
   usleep(50000);
   while (ctx->total > 0)
   {
      ssize_t n = read(ctx->fd, buf, sizeof(buf));
      if (n <= 0)
         break;
      ctx->total -= (size_t)n;
   }
   return NULL;
}

int agent_load_config(agent_config_t *cfg)
{
   memset(cfg, 0, sizeof(*cfg));
   return 0;
}

void agent_http_init(void)
{
}

int agent_run(sqlite3 *db, agent_config_t *cfg, const char *role, const char *system_prompt,
              const char *prompt, int max_tokens, agent_result_t *result)
{
   (void)db;
   (void)cfg;
   (void)role;
   (void)system_prompt;
   (void)prompt;
   (void)max_tokens;
   memset(result, 0, sizeof(*result));
   return 0;
}

int agent_run_with_tools(sqlite3 *db, agent_config_t *cfg, const char *role,
                         const char *system_prompt, const char *prompt, int max_tokens,
                         agent_result_t *result)
{
   (void)db;
   (void)cfg;
   (void)role;
   (void)system_prompt;
   (void)prompt;
   (void)max_tokens;
   memset(result, 0, sizeof(*result));
   return 0;
}

char *dispatch_tool_call(const char *name, const char *arguments_json, int timeout_ms)
{
   (void)name;
   (void)arguments_json;
   (void)timeout_ms;
   return strdup("ok");
}

int pre_tool_check(sqlite3 *db, const char *tool_name, const char *tool_input,
                   session_state_t *state, const char *guardrail_mode, const char *cwd, char *msg,
                   size_t msg_len)
{
   (void)db;
   (void)tool_name;
   (void)tool_input;
   (void)state;
   (void)guardrail_mode;
   (void)cwd;
   if (msg_len > 0)
      msg[0] = '\0';
   return 0;
}

void session_state_load(session_state_t *state, const char *path)
{
   (void)path;
   memset(state, 0, sizeof(*state));
}

void session_state_save(const session_state_t *state, const char *path)
{
   (void)state;
   (void)path;
}

int compute_pool_submit(compute_pool_t *pool, void (*fn)(void *), void *arg)
{
   (void)pool;
   (void)fn;
   (void)arg;
   return 0;
}

int server_send_response(server_conn_t *conn, cJSON *resp)
{
   (void)conn;
   cJSON_Delete(resp);
   return 0;
}

int server_send_error(server_conn_t *conn, const char *message, const char *request_id)
{
   (void)conn;
   (void)message;
   (void)request_id;
   return 0;
}

static void test_mailbox_lifecycle(void)
{
   delegation_mailbox_t *mb = mailbox_acquire("deleg-1");
   assert(mb != NULL);
   assert(mailbox_find("deleg-1") == mb);
   mailbox_release(mb);
   assert(mailbox_find("deleg-1") == NULL);
}

static void test_reply_wakeup(void)
{
   delegation_mailbox_t *mb = mailbox_acquire("deleg-2");
   assert(mb != NULL);
   pthread_t thread;
   assert(pthread_create(&thread, NULL, waiter_thread, mb) == 0);
   usleep(20000);
   mailbox_reply(mb, "parent reply");
   char *out = NULL;
   assert(pthread_join(thread, (void **)&out) == 0);
   assert(strcmp(out, "parent reply") == 0);
   free(out);
   mailbox_release(mb);
}

static void test_timeout_and_no_mailbox(void)
{
   delegation_mailbox_t *mb = mailbox_acquire("deleg-timeout");
   assert(mb != NULL);
   char reply[32];
   assert(mailbox_wait(mb, reply, sizeof(reply), 0) != 0);
   mailbox_release(mb);

   tl_mailbox = NULL;
   tl_deleg_db = NULL;
   assert(delegation_request_input("question?") == NULL);
}

static void test_message_recording(void)
{
   sqlite3 *db = db_open(":memory:");
   assert(db != NULL);

   delegation_mailbox_t *mb = mailbox_acquire("deleg-db");
   assert(mb != NULL);
   tl_mailbox = mb;
   tl_deleg_db = db;

   deleg_msg_record(db, "deleg-db", "delegate_to_parent", "question");
   deleg_msg_record(db, "deleg-db", "parent_to_delegate", "answer");

   sqlite3_stmt *stmt = db_prepare(db, "SELECT direction, content FROM delegation_messages "
                                       "WHERE delegation_id = ? ORDER BY id");
   assert(stmt != NULL);
   sqlite3_bind_text(stmt, 1, "deleg-db", -1, SQLITE_TRANSIENT);
   assert(sqlite3_step(stmt) == SQLITE_ROW);
   assert(strcmp((const char *)sqlite3_column_text(stmt, 0), "delegate_to_parent") == 0);
   assert(strcmp((const char *)sqlite3_column_text(stmt, 1), "question") == 0);
   assert(sqlite3_step(stmt) == SQLITE_ROW);
   assert(strcmp((const char *)sqlite3_column_text(stmt, 0), "parent_to_delegate") == 0);
   assert(strcmp((const char *)sqlite3_column_text(stmt, 1), "answer") == 0);
   sqlite3_reset(stmt);

   tl_mailbox = NULL;
   tl_deleg_db = NULL;
   mailbox_release(mb);
   db_stmt_cache_clear();
   db_close(db);
}

static void test_write_all_handles_eagain(void)
{
   int fds[2];
   assert(pipe(fds) == 0);
   int flags = fcntl(fds[1], F_GETFL, 0);
   assert(flags >= 0);
   assert(fcntl(fds[1], F_SETFL, flags | O_NONBLOCK) == 0);

   char filler[4096];
   memset(filler, 'x', sizeof(filler));
   while (write(fds[1], filler, sizeof(filler)) > 0)
   {
   }
   assert(errno == EAGAIN || errno == EWOULDBLOCK);

   char payload[16384];
   memset(payload, 'y', sizeof(payload));
   drain_ctx_t ctx = {.fd = fds[0], .total = sizeof(payload) + sizeof(filler)};
   pthread_t drain_thread;
   assert(pthread_create(&drain_thread, NULL, drain_pipe_thread, &ctx) == 0);
   assert(write_all(fds[1], payload, sizeof(payload)) == 0);
   close(fds[1]);
   pthread_join(drain_thread, NULL);
   close(fds[0]);
}

int main(void)
{
   test_mailbox_lifecycle();
   test_reply_wakeup();
   test_timeout_and_no_mailbox();
   test_message_recording();
   test_write_all_handles_eagain();
   printf("server_compute: all tests passed\n");
   return 0;
}
