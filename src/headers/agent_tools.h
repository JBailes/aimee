#ifndef DEC_AGENT_TOOLS_H
#define DEC_AGENT_TOOLS_H 1

#include "agent_types.h"

/* Tool execution (Unix only) */
char *tool_bash(const char *command, int timeout_ms);
char *tool_read_file(const char *path, int offset, int limit);
char *tool_write_file(const char *path, const char *content);
char *tool_list_files(const char *path, const char *pattern);
char *tool_verify(const char *check_type, const char *target, const char *expected);
char *tool_git_log(const char *repo_path, int count);
char *tool_grep(const char *path, const char *pattern, int max_results);
char *tool_git_diff(const char *repo_path, const char *ref);
char *tool_git_status(const char *repo_path);
char *tool_env_get(const char *name);
char *tool_test(const char *path, const char *check);
char *tool_request_input(const char *question);
char *tool_code_search(const char *query, const char *project, int max_results);
char *dispatch_tool_call(const char *name, const char *arguments_json, int timeout_ms);

/* Tool definition builders */
struct cJSON *build_tools_array(void);
struct cJSON *build_tools_array_responses(void);
struct cJSON *build_tools_array_anthropic(void);

/* Execution checkpoints */
typedef struct
{
   int step_id;
   char path[MAX_PATH_LEN];
   char *original_content;
   char command[4096];
   int rolled_back;
} exec_checkpoint_t;

int exec_checkpoint_capture(exec_checkpoint_t *cp, const char *path);
int exec_checkpoint_restore(const exec_checkpoint_t *cp);
void exec_checkpoint_free(exec_checkpoint_t *cp);

/* Per-invocation checkpoint context (replaces global state) */
typedef struct
{
   exec_checkpoint_t checkpoints[AGENT_MAX_CHECKPOINTS];
   int count;
} checkpoint_ctx_t;

checkpoint_ctx_t *checkpoint_ctx_new(void);
void checkpoint_ctx_push(checkpoint_ctx_t *ctx, const char *path);
void checkpoint_ctx_rollback_all(checkpoint_ctx_t *ctx);
void checkpoint_ctx_clear(checkpoint_ctx_t *ctx);
void checkpoint_ctx_free(checkpoint_ctx_t *ctx);

/* Dispatch with per-invocation checkpoint context.
 * ctx may be NULL (checkpoints disabled for that call). */
char *dispatch_tool_call_ctx(const char *name, const char *arguments_json, int timeout_ms,
                             checkpoint_ctx_t *ctx);

/* Legacy global-state wrappers (deprecated, thin shims around a process-global ctx) */
void exec_checkpoint_push(const char *path);
void exec_checkpoints_rollback_all(void);
void exec_checkpoints_clear(void);

#endif /* DEC_AGENT_TOOLS_H */
