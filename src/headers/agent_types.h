#ifndef DEC_AGENT_TYPES_H
#define DEC_AGENT_TYPES_H 1

#include <pthread.h>
#include <sys/types.h>

/* Forward declaration for cJSON (used by plan API). */
struct cJSON;

#define MAX_AGENTS                16
#define MAX_AGENT_ROLES           16
#define MAX_AGENT_NAME            64
#define MAX_ENDPOINT_LEN          512
#define MAX_MODEL_LEN             128
#define MAX_API_KEY_LEN           4096
#define MAX_AUTH_CMD_LEN          512
#define MAX_FALLBACK              8
#define AGENT_DEFAULT_TIMEOUT_MS  180000
#define AGENT_DEFAULT_MAX_TOKENS  4096
#define MAX_EXEC_ROLES            8
#define MAX_EXEC_PROMPT_LEN       4096
#define AGENT_DEFAULT_MAX_TURNS   20
#define AGENT_DEFAULT_MAX_PARALLEL 3
#define AGENT_TOOL_OUTPUT_MAX     (4 * 1024)
#define AGENT_TOOL_OUTPUT_RAW_MAX (32 * 1024)
#define AGENT_MAX_LIST_FILES      500
#define AGENT_MAX_TOOL_CALLS      16
#define AGENT_MAX_NET_HOSTS       64
#define AGENT_MAX_NETWORKS        8
#define AGENT_MAX_TUNNELS         8
#define AGENT_CONTEXT_BUDGET      16000
#define AGENT_CACHE_TTL_SECONDS   300
#define AGENT_MAX_PLAN_STEPS      32
#define AGENT_MAX_PLAN_DEPS       8
#define AGENT_MAX_CHECKPOINTS     32
#define AGENT_MAX_EVAL_TASKS      64
#define AGENT_MAX_COORD_AGENTS    4

typedef struct
{
   char name[64];
   char ip[64];
   char user[32];
   int port;
   char desc[256];
   char tunnel[64]; /* optional: name of tunnel for this host */
} agent_net_host_t;

typedef struct
{
   char name[64];
   char cidr[32];
   char desc[256];
} agent_net_def_t;

typedef enum
{
   TUNNEL_STATE_IDLE = 0,
   TUNNEL_STATE_CONNECTING,
   TUNNEL_STATE_ACTIVE,
   TUNNEL_STATE_RECONNECTING,
   TUNNEL_STATE_FAILED,
   TUNNEL_STATE_STOPPED
} agent_tunnel_state_t;

typedef struct
{
   /* Config (from JSON) */
   char name[64];
   char relay_ssh[512]; /* e.g. "ssh relay@relay.example.com" */
   char relay_key[MAX_PATH_LEN];
   char target_host[64];  /* internal target, e.g. "192.168.1.101" */
   int target_port;       /* internal target port, e.g. 22 */
   int reconnect_delay_s; /* seconds between retries, default 5 */
   int max_reconnects;    /* 0 = unlimited */

   /* Runtime state (not serialized) */
   agent_tunnel_state_t state;
   int allocated_port;
   pid_t ssh_pid;
   pthread_t monitor_thread;
   int reconnect_count;
   char error[256];
   char effective_entry[512]; /* computed: "ssh -p <port> user@relay" */
} agent_tunnel_t;

typedef struct
{
   agent_tunnel_t tunnels[AGENT_MAX_TUNNELS];
   int tunnel_count;
   pthread_mutex_t lock;
   volatile int shutdown;
} agent_tunnel_mgr_t;

typedef struct
{
   char ssh_entry[512];
   char ssh_key[MAX_PATH_LEN];
   agent_net_host_t hosts[AGENT_MAX_NET_HOSTS];
   int host_count;
   agent_net_def_t networks[AGENT_MAX_NETWORKS];
   int network_count;
   agent_tunnel_mgr_t *tunnel_mgr; /* optional, NULL when no tunnels */
} agent_network_t;

typedef struct
{
   char name[MAX_AGENT_NAME];
   char endpoint[MAX_ENDPOINT_LEN];
   char model[MAX_MODEL_LEN];
   char api_key[MAX_API_KEY_LEN];
   char auth_cmd[MAX_AUTH_CMD_LEN];
   char auth_type[16];
   char provider[16];
   char roles[MAX_AGENT_ROLES][32];
   int role_count;
   int cost_tier;
   int max_tokens;
   int timeout_ms;
   int enabled;
   int tools_enabled;
   int max_turns;
   int max_parallel;
   char exec_roles[MAX_EXEC_ROLES][32];
   int exec_role_count;
   char exec_system_prompt[MAX_EXEC_PROMPT_LEN];
   char extra_headers[256]; /* newline-separated extra HTTP headers, e.g. ChatGPT-Account-ID */
   char fallback_model[MAX_MODEL_LEN];
} agent_t;

typedef struct
{
   agent_t agents[MAX_AGENTS];
   int agent_count;
   char default_agent[MAX_AGENT_NAME];
   char fallback_chain[MAX_FALLBACK][MAX_AGENT_NAME];
   int fallback_count;
   agent_network_t network;
   agent_tunnel_mgr_t tunnel_mgr;
} agent_config_t;

typedef struct
{
   const char *role;
   const char *system_prompt;
   const char *user_prompt;
   int max_tokens;
   double temperature;
} agent_task_t;

typedef struct
{
   char agent_name[MAX_AGENT_NAME];
   char *response;
   int prompt_tokens;
   int completion_tokens;
   int latency_ms;
   int success;
   char error[512];
   int turns;
   int tool_calls;
   int confidence;
   int abstained;
   char abstain_reason[512];
} agent_result_t;

/* Structured outcome classification for agent executions */
typedef enum
{
   OUTCOME_SUCCESS = 0,
   OUTCOME_PARTIAL,
   OUTCOME_FAILURE,
   OUTCOME_ERROR
} outcome_type_t;

typedef struct
{
   outcome_type_t outcome;
   char reason[256];
   int turns_used;
   int tools_called;
   int64_t tokens_used;
   char tool_error_pattern[128]; /* repeated tool+error key for anti-pattern extraction */
} agent_outcome_t;

typedef struct
{
   char name[MAX_AGENT_NAME];
   int total_calls;
   int total_prompt_tokens;
   int total_completion_tokens;
   int avg_latency_ms;
   double success_rate;
} agent_stats_t;

/* Task type classification for context assembly */
typedef enum
{
   TASK_TYPE_GENERAL = 0,
   TASK_TYPE_BUG_FIX,
   TASK_TYPE_REFACTOR,
   TASK_TYPE_FEATURE,
   TASK_TYPE_REVIEW,
   TASK_TYPE_TEST,
   TASK_TYPE_COUNT
} task_type_t;

/* Context category weights per task type */
typedef enum
{
   CTX_WEIGHT_LOW = 10,
   CTX_WEIGHT_MED = 25,
   CTX_WEIGHT_HIGH = 40
} ctx_weight_t;

#endif /* DEC_AGENT_TYPES_H */
