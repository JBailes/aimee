#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include "aimee.h"
#include "log.h"
#include "server.h"

static const char *g_last_handler = NULL;

static char *read_all(int fd)
{
   char buf[8192];
   size_t used = 0;
   char *out = calloc(1, sizeof(buf));
   assert(out != NULL);
   ssize_t n;
   while ((n = read(fd, buf + used, sizeof(buf) - 1 - used)) > 0)
      used += (size_t)n;
   memcpy(out, buf, used);
   out[used] = '\0';
   return out;
}

static cJSON *dispatch_json(server_ctx_t *ctx, server_conn_t *conn, const char *msg, size_t len)
{
   int fds[2];
   assert(pipe(fds) == 0);
   conn->fd = fds[1];
   assert(server_dispatch(ctx, conn, msg, len) == 0);
   close(fds[1]);
   char *resp = read_all(fds[0]);
   close(fds[0]);
   cJSON *json = cJSON_Parse(resp);
   assert(json != NULL);
   free(resp);
   return json;
}

static int stub_handler(server_conn_t *conn, const char *name)
{
   g_last_handler = name;
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "route", name);
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

int platform_evloop_del(platform_evloop_t *loop, int fd)
{
   (void)loop;
   (void)fd;
   return 0;
}

int platform_evloop_mod(platform_evloop_t *loop, int fd, uint32_t events)
{
   (void)loop;
   (void)fd;
   (void)events;
   return 0;
}

int platform_evloop_add(platform_evloop_t *loop, int fd, uint32_t events)
{
   (void)loop;
   (void)fd;
   (void)events;
   return 0;
}

int platform_evloop_create(platform_evloop_t *loop)
{
   (void)loop;
   return 0;
}

int platform_evloop_wait(platform_evloop_t *loop, platform_event_t *events, int max_events,
                         int timeout_ms)
{
   (void)loop;
   (void)events;
   (void)max_events;
   (void)timeout_ms;
   return 0;
}

void platform_evloop_destroy(platform_evloop_t *loop)
{
   (void)loop;
}

int platform_ipc_accept(int listen_fd)
{
   (void)listen_fd;
   return -1;
}

int platform_ipc_peer_cred(int fd, uid_t *uid, gid_t *gid, pid_t *pid)
{
   (void)fd;
   if (uid)
      *uid = 0;
   if (gid)
      *gid = 0;
   if (pid)
      *pid = 0;
   return 0;
}

int platform_ipc_listen(const char *path)
{
   (void)path;
   return -1;
}

int platform_ipc_probe(const char *path)
{
   (void)path;
   return 0;
}

sqlite3 *db_open(const char *path)
{
   (void)path;
   return NULL;
}

sqlite3 *db_open_fast(const char *path)
{
   (void)path;
   return NULL;
}

void db_apply_pragmas(sqlite3 *db, db_mode_t mode)
{
   (void)db;
   (void)mode;
}

int compute_pool_init(compute_pool_t *pool, int num_threads)
{
   (void)pool;
   (void)num_threads;
   return 0;
}

void compute_pool_shutdown(compute_pool_t *pool)
{
   (void)pool;
}

int server_load_token(server_ctx_t *ctx)
{
   (void)ctx;
   return 0;
}

void aimee_log(log_level_t level, const char *module, const char *fmt, ...)
{
   (void)level;
   (void)module;
   (void)fmt;
}

uint32_t server_capability_for_method(const char *method)
{
   if (strcmp(method, "tool.execute") == 0)
      return CAP_TOOL_EXECUTE;
   return 0;
}

const method_policy_t *server_policy_for_method(const char *method)
{
   static const method_policy_t policy = {"tool.execute", CAP_TOOL_EXECUTE, "execute tool"};
   return strcmp(method, "tool.execute") == 0 ? &policy : NULL;
}

int handle_auth(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "auth");
}
int handle_session_create(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "session.create");
}
int handle_session_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "session.list");
}
int handle_session_get(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "session.get");
}
int handle_session_close(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "session.close");
}
int handle_memory_search(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "memory.search");
}
int handle_memory_store(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "memory.store");
}
int handle_memory_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "memory.list");
}
int handle_memory_get(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "memory.get");
}
int handle_index_find(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "index.find");
}
int handle_index_blast_radius(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "index.blast_radius");
}
int handle_blast_radius_preview(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "blast_radius.preview");
}
int handle_index_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "index.list");
}
int handle_rules_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "rules.list");
}
int handle_rules_generate(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "rules.generate");
}
int handle_wm_set(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "wm.set");
}
int handle_wm_get(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "wm.get");
}
int handle_wm_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "wm.list");
}
int handle_wm_context(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "wm.context");
}
int handle_attempt_record(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "attempt.record");
}
int handle_attempt_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "attempt.list");
}
int handle_dashboard_metrics(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "dashboard.metrics");
}
int handle_dashboard_delegations(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "dashboard.delegations");
}
int handle_workspace_context(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "workspace.context");
}
int handle_tool_execute(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "tool.execute");
}
int handle_delegate(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "delegate");
}
int handle_delegate_reply(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "delegate.reply");
}
int handle_chat_send_stream(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "chat.send_stream");
}
int handle_cli_forward(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "cli.forward");
}

int handle_mcp_call(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "mcp.call");
}

int is_safe_id(const char *s)
{
   (void)s;
   return 1;
}

int config_load(config_t *cfg)
{
   memset(cfg, 0, sizeof(*cfg));
   return 0;
}

const char *config_output_dir(void)
{
   return "/tmp";
}

const char *config_guardrail_mode(const config_t *cfg)
{
   (void)cfg;
   return "off";
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

void post_tool_update(sqlite3 *db, const char *tool_name, const char *tool_input)
{
   (void)db;
   (void)tool_name;
   (void)tool_input;
}

static void test_invalid_json(void)
{
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   cJSON *json = dispatch_json(ctx, conn, "{", 1);
   assert(strcmp(cJSON_GetObjectItem(json, "status")->valuestring, "error") == 0);
   assert(strcmp(cJSON_GetObjectItem(json, "message")->valuestring, "invalid JSON") == 0);
   cJSON_Delete(json);
   free(conn);
   free(ctx);
}

static void test_missing_method(void)
{
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   cJSON *json =
       dispatch_json(ctx, conn, "{\"request_id\":\"r1\"}", strlen("{\"request_id\":\"r1\"}"));
   assert(strcmp(cJSON_GetObjectItem(json, "message")->valuestring, "missing method") == 0);
   cJSON_Delete(json);
   free(conn);
   free(ctx);
}

static void test_oversized_payload(void)
{
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   size_t size = LIMIT_MEMORY + 64;
   char *msg = malloc(size + 1);
   assert(msg != NULL);
   snprintf(msg, size + 1, "{\"method\":\"memory.list\",\"pad\":\"");
   size_t used = strlen(msg);
   memset(msg + used, 'a', size - used - 2);
   msg[size - 2] = '"';
   msg[size - 1] = '}';
   msg[size] = '\0';
   cJSON *json = dispatch_json(ctx, conn, msg, size);
   assert(strstr(cJSON_GetObjectItem(json, "message")->valuestring, "PAYLOAD_TOO_LARGE") != NULL);
   cJSON_Delete(json);
   free(msg);
   free(conn);
   free(ctx);
}

static void test_unknown_method(void)
{
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   cJSON *json = dispatch_json(ctx, conn, "{\"method\":\"unknown.method\"}",
                               strlen("{\"method\":\"unknown.method\"}"));
   assert(strcmp(cJSON_GetObjectItem(json, "message")->valuestring, "unknown method") == 0);
   cJSON_Delete(json);
   free(conn);
   free(ctx);
}

static void test_authz_denied_shape(void)
{
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   conn->peer_uid = 4242;
   conn->capabilities = 0;
   cJSON *json = dispatch_json(ctx, conn, "{\"method\":\"tool.execute\"}",
                               strlen("{\"method\":\"tool.execute\"}"));
   assert(strcmp(cJSON_GetObjectItem(json, "code")->valuestring, "AUTHZ_DENIED") == 0);
   assert(strcmp(cJSON_GetObjectItem(json, "method")->valuestring, "tool.execute") == 0);
   assert(strstr(cJSON_GetObjectItem(json, "principal")->valuestring, "uid:4242") != NULL);
   assert(cJSON_GetObjectItem(json, "required_caps") != NULL);
   assert(cJSON_GetObjectItem(json, "held_caps") != NULL);
   cJSON_Delete(json);
   free(conn);
   free(ctx);
}

static void test_routing(void)
{
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   conn->capabilities = CAPS_AUTHENTICATED;

   cJSON *json = dispatch_json(ctx, conn, "{\"method\":\"server.info\"}",
                               strlen("{\"method\":\"server.info\"}"));
   assert(cJSON_GetObjectItem(json, "protocol_version") != NULL);
   cJSON_Delete(json);

   json = dispatch_json(ctx, conn, "{\"method\":\"session.list\"}",
                        strlen("{\"method\":\"session.list\"}"));
   assert(strcmp(cJSON_GetObjectItem(json, "route")->valuestring, "session.list") == 0);
   assert(strcmp(g_last_handler, "session.list") == 0);
   cJSON_Delete(json);

   json = dispatch_json(ctx, conn, "{\"method\":\"cli.forward\"}",
                        strlen("{\"method\":\"cli.forward\"}"));
   assert(strcmp(cJSON_GetObjectItem(json, "route")->valuestring, "cli.forward") == 0);
   assert(strcmp(g_last_handler, "cli.forward") == 0);
   cJSON_Delete(json);
   free(conn);
   free(ctx);
}

int main(void)
{
   test_invalid_json();
   test_missing_method();
   test_oversized_payload();
   test_unknown_method();
   test_authz_denied_shape();
   test_routing();
   printf("server_dispatch: all tests passed\n");
   return 0;
}
