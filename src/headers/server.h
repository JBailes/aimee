#ifndef DEC_SERVER_H
#define DEC_SERVER_H 1

#include <sqlite3.h>
#include <stdint.h>
#include <sys/types.h>
#include "compute_pool.h"
#include "platform_event.h"

/* Forward declaration */
typedef struct cJSON cJSON;

#define SERVER_PROTOCOL_VERSION 1
#define SERVER_MAX_CONNECTIONS  64
#define SERVER_MAX_MSG_SIZE     (16 * 1024 * 1024) /* 16MB */
#define SERVER_READ_BUF_SIZE    65536
#define SERVER_WRITE_BUF_SIZE   262144 /* 256KB */
#define SERVER_DEFAULT_SOCKET   "aimee.sock"
#define SERVER_TOKEN_FILE       "server.token"
#define SERVER_TOKEN_LEN        64 /* 64 hex bytes = 32 raw bytes */
#define SERVER_LISTEN_BACKLOG   16
#define CONN_WRITE_DEADLINE_MS  10000 /* 10 seconds */

/* Per-method payload size limits */
#define LIMIT_MEMORY  (256 * 1024)       /* 256KB for memory operations */
#define LIMIT_TOOL    (4 * 1024 * 1024)  /* 4MB for tool I/O */
#define LIMIT_DELEGATE (1 * 1024 * 1024) /* 1MB for delegation */
#define LIMIT_CHAT    (512 * 1024)       /* 512KB for chat messages */
#define LIMIT_DEFAULT (256 * 1024)       /* 256KB default */

/* JSON framing limits */
#define JSON_MAX_DEPTH  32  /* maximum nesting depth */
#define JSON_MAX_FIELDS 256 /* maximum fields per object */

/* Capability flags (bitmask) */
#define CAP_CHAT           (1u << 0)
#define CAP_DELEGATE       (1u << 1)
#define CAP_TOOL_EXECUTE   (1u << 2)
#define CAP_TOOL_BASH      (1u << 3)
#define CAP_TOOL_WRITE     (1u << 4)
#define CAP_MEMORY_READ    (1u << 5)
#define CAP_MEMORY_WRITE   (1u << 6)
#define CAP_RULES_READ     (1u << 7)
#define CAP_RULES_ADMIN    (1u << 8)
#define CAP_DESCRIBE_READ  (1u << 9)
#define CAP_DESCRIBE_ADMIN (1u << 10)
#define CAP_INDEX_READ     (1u << 11)
#define CAP_INDEX_ADMIN    (1u << 12)
#define CAP_SESSION_READ   (1u << 13)
#define CAP_SESSION_ADMIN  (1u << 14)
#define CAP_DASHBOARD_READ (1u << 15)

/* Composite capability sets */
#define CAPS_ALL 0xFFFFu
#define CAPS_READ_ONLY                                                                             \
   (CAP_CHAT | CAP_MEMORY_READ | CAP_RULES_READ | CAP_INDEX_READ | CAP_SESSION_READ |              \
    CAP_DASHBOARD_READ | CAP_DESCRIBE_READ)
#define CAPS_AUTHENTICATED                                                                         \
   (CAPS_READ_ONLY | CAP_DELEGATE | CAP_TOOL_EXECUTE | CAP_TOOL_BASH | CAP_TOOL_WRITE |            \
    CAP_MEMORY_WRITE | CAP_SESSION_ADMIN)

/* Method-to-capability policy entry */
typedef struct
{
   const char *method;      /* exact match, or prefix with trailing '*' */
   uint32_t required_caps;  /* capability bitmask (0 = no auth required) */
   const char *description; /* human-readable for audit/logging */
} method_policy_t;

/* Declarative method registry (defined in server_auth.c) */
extern const method_policy_t method_registry[];
extern const int method_registry_count;

/* Per-connection state */
typedef struct
{
   int fd;
   platform_evloop_t *evloop; /* pointer to server's event loop for OUT registration */
   uid_t peer_uid;
   gid_t peer_gid;
   pid_t peer_pid;
   uint32_t capabilities;
   char read_buf[SERVER_READ_BUF_SIZE];
   size_t read_len;
   char write_buf[SERVER_WRITE_BUF_SIZE];
   size_t write_len;          /* number of pending bytes in write_buf */
   size_t write_pos;          /* offset of first pending byte in write_buf */
   int64_t write_deadline_ms; /* monotonic ms when pending write must complete, 0 = no pending */
   sqlite3 *db;
} server_conn_t;

/* Idle timeout for non-persistent servers (seconds).
 * Counts from when active_sessions last hit 0, not from last CLI command. */
#define SERVER_IDLE_TIMEOUT 1800 /* 30 minutes */

/* Server context */
typedef struct
{
   int listen_fd;
   platform_evloop_t evloop;
   char socket_path[4096];
   char token[SERVER_TOKEN_LEN * 2 + 1]; /* hex string */
   server_conn_t conns[SERVER_MAX_CONNECTIONS];
   int conn_count;
   sqlite3 *db;
   compute_pool_t pool;
   volatile int running;
   time_t start_time;
   int active_sessions; /* refcount of live sessions (atomic) */
   int persistent;      /* if >0, ignore shutdown requests and idle timeout */
   time_t
       last_session_end; /* when active_sessions last hit 0; 0 = sessions active or fresh start */
} server_ctx_t;

/* Lifecycle */
int server_init(server_ctx_t *ctx, const char *socket_path);
int server_run(server_ctx_t *ctx);
void server_shutdown(server_ctx_t *ctx);

/* Method dispatch */
int server_dispatch(server_ctx_t *ctx, server_conn_t *conn, const char *msg, size_t msg_len);

/* Response helpers (shared across handler files) */
int server_send_response(server_conn_t *conn, cJSON *resp);
int server_send_error(server_conn_t *conn, const char *message, const char *request_id);

/* Auth (server_auth.c) */
int server_load_token(server_ctx_t *ctx);
uint32_t server_capability_for_method(const char *method);
const method_policy_t *server_policy_for_method(const char *method);
int handle_auth(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);

/* Session handlers (server_session.c) */
int handle_session_create(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_session_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_session_get(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_session_close(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);

/* State handlers (server_state.c) */
int handle_memory_search(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_memory_store(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_memory_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_memory_get(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_index_find(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_index_blast_radius(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_blast_radius_preview(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_index_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_rules_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_rules_generate(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_wm_set(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_wm_get(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_wm_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_wm_context(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_attempt_record(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_attempt_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_dashboard_metrics(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_dashboard_delegations(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_workspace_context(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);

/* Compute handlers (server_compute.c) */
int handle_tool_execute(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_delegate(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_delegate_reply(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_chat_send_stream(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);

/* MCP proxy handler (server_mcp.c) */
int handle_mcp_call(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);

/* Forward handler (server_forward.c) */
int handle_cli_forward(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);

#endif /* DEC_SERVER_H */
