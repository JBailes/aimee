#ifndef DEC_AGENT_EXEC_H
#define DEC_AGENT_EXEC_H 1

#include "agent_types.h"

/* Execution */
int agent_execute_with_tools(sqlite3 *db, const agent_t *agent, const agent_network_t *network,
                             const char *system_prompt, const char *user_prompt, int max_tokens,
                             double temperature, agent_result_t *out);

int agent_execute(sqlite3 *db, const agent_t *agent, const char *system_prompt,
                  const char *user_prompt, int max_tokens, double temperature, agent_result_t *out);

int agent_run(sqlite3 *db, agent_config_t *cfg, const char *role, const char *system_prompt,
              const char *user_prompt, int max_tokens, agent_result_t *out);

/* Like agent_run but forces tool execution regardless of agent config */
int agent_run_with_tools(sqlite3 *db, agent_config_t *cfg, const char *role,
                         const char *system_prompt, const char *user_prompt, int max_tokens,
                         agent_result_t *out);

int agent_run_parallel(sqlite3 *db, agent_config_t *cfg, agent_task_t *tasks, int task_count,
                       agent_result_t *out);

/* Logging */
void agent_log_call(sqlite3 *db, const agent_result_t *result, const char *role);
int agent_get_stats(sqlite3 *db, const char *name, agent_stats_t *out, int max);

/* Task type classification */
task_type_t task_type_classify(const char *prompt);
const char *task_type_name(task_type_t type);

/* Context assembly */
char *agent_build_exec_context(sqlite3 *db, const agent_t *agent, const agent_network_t *network,
                               const char *custom_prompt);
void agent_print_context(sqlite3 *db, const agent_config_t *cfg);

/* Ephemeral SSH */
int agent_ssh_setup(const agent_network_t *network, char *key_path_out, size_t key_path_len,
                    char *session_id_out, size_t session_id_len);
void agent_ssh_cleanup(const agent_network_t *network, const char *key_path,
                       const char *session_id);

/* Policy, trace, confidence */
int tool_validate(sqlite3 *db, const char *tool_name, const char *args_json, char *err_out,
                  size_t err_len);
const char *tool_side_effect(sqlite3 *db, const char *tool_name);
void agent_trace_log(sqlite3 *db, int plan_id, int turn, const char *direction, const char *content,
                     const char *tool_name, const char *tool_args, const char *tool_result,
                     const char *context_hash);
int agent_estimate_confidence(const char *response_text);
int policy_check_tool(const char *tool_name, const char *side_effect, const char *args_json,
                      char *reason_out, size_t reason_len);
int policy_load(void);

/* Metrics, introspection, manifests, contract */
void agent_write_metrics(sqlite3 *db);
void agent_introspect_env(sqlite3 *db);
void agent_write_manifest(sqlite3 *db, const char *run_id, const agent_result_t *result,
                          const char *role);
char *agent_load_project_contract(const char *project_root);
char *agent_compress_tool_result(const char *raw, size_t raw_len);

/* Merge consecutive same-role messages in-place. Returns merge count. */
int messages_compact_consecutive(struct cJSON *messages);

/* Provider health */
typedef enum
{
   PROVIDER_ERR_NONE = 0,
   PROVIDER_ERR_NETWORK,    /* connection refused, DNS failure, timeout */
   PROVIDER_ERR_AUTH,       /* 401, 403 */
   PROVIDER_ERR_RATE_LIMIT, /* 429 */
   PROVIDER_ERR_SERVER,     /* 5xx */
   PROVIDER_ERR_CLIENT,     /* other 4xx */
   PROVIDER_ERR_UNKNOWN
} provider_err_class_t;

typedef struct
{
   int available;         /* 1=ok, 0=unreachable, -1=unknown */
   int64_t last_check_ms; /* monotonic timestamp of last check */
   int last_http_status;  /* last HTTP status code, or -1 for network error */
   char error[256];       /* last error message */
} provider_health_t;

provider_err_class_t provider_classify_error(int http_status);
const char *provider_error_message(provider_err_class_t cls);
void provider_health_update(const char *provider_name, int http_status);
const provider_health_t *provider_health_get(const char *provider_name);

/* HTTP */
int agent_http_post(const char *url, const char *auth_header, const char *body, char **response_buf,
                    int timeout_ms, const char *extra_headers);

/* Streaming callback: called for each data chunk. Return 0 to continue, non-zero to abort. */
typedef int (*agent_http_stream_cb)(const char *data, size_t len, void *userdata);

int agent_http_post_stream(const char *url, const char *auth_header, const char *body,
                           agent_http_stream_cb callback, void *userdata, int timeout_ms,
                           const char *extra_headers);
int agent_http_post_form(const char *url, const char *body, char **response_buf, int timeout_ms);
void agent_http_init(void);
void agent_http_cleanup(void);

#endif /* DEC_AGENT_EXEC_H */
