#ifndef DEC_GUARDRAILS_H
#define DEC_GUARDRAILS_H 1

#include <pthread.h>
#include <stdatomic.h>

#define MAX_WORKTREES  16
#define MAX_SEEN_PATHS 64
#define MAX_SEEN_LEN   512

typedef struct
{
   char path[MAX_PATH_LEN];
   severity_t severity;
   char reason[256];
} classification_t;

/* Simple worktree mapping: git repo root -> sibling worktree path */
typedef struct
{
   char git_root[MAX_PATH_LEN];    /* original repo root (e.g. /root/dev/aimee) */
   char worktree_path[MAX_PATH_LEN]; /* sibling worktree (e.g. /root/dev/.aimee-aimee-abc12345) */
} worktree_mapping_t;

typedef struct
{
   char seen_paths[MAX_SEEN_PATHS][MAX_SEEN_LEN];
   int seen_count;
   char session_mode[16];
   char guardrail_mode[16];
   int64_t active_task_id;
   int hook_call_count; /* increments each pre_tool_check call for diagnostics */
   int dirty;

   /* Active worktree mappings for this session */
   worktree_mapping_t worktrees[MAX_WORKTREES];
   int worktree_count;
} session_state_t;

/* Check if a filename matches sensitive patterns (substring match). */
int is_sensitive_file(const char *path);

/* Classify a file path by sensitivity and blast radius. */
classification_t classify_path(sqlite3 *db, const char *file_path);

/* Pre-tool check. Returns exit code (0 = allow, 2 = block).
 * Message written to msg_buf if blocked. */
int pre_tool_check(sqlite3 *db, const char *tool_name, const char *input_json,
                   session_state_t *state, const char *guardrail_mode, const char *cwd,
                   char *msg_buf, size_t msg_len);

/* Normalize provider/internal tool names into guardrail-facing categories. */
const char *guardrails_canonical_tool_name(const char *tool_name);

/* Post-tool update: re-index edited files. */
void post_tool_update(sqlite3 *db, const char *tool_name, const char *input_json);

/* Load session state from file. */
void session_state_load(session_state_t *state, const char *path);

/* Save session state to file (only if dirty). */
void session_state_save(const session_state_t *state, const char *path);

/* Force save regardless of dirty flag. */
void session_state_force_save(const session_state_t *state, const char *path);

/* Check if a command is a write/destructive operation. */
int is_write_command(const char *command);

/* Check if a tool is a shell/bash provider. */
int is_shell_tool(const char *tool);

/* Normalize a relative path against cwd. Result written to buf. */
char *normalize_path(const char *path, const char *cwd, char *buf, size_t buf_len);

/* Resolve the git repository root for a directory. Returns 0 on success. */
int git_repo_root(const char *dir, char *out_root, size_t out_len);

/* Compute the expected sibling worktree path for a git repo and session.
 * Writes result to wt_buf. Returns 0 on success. */
int worktree_sibling_path(const char *git_root, const char *session_id,
                          char *wt_buf, size_t wt_len);

/* Check if a path is already inside an aimee worktree (contains /.aimee- component). */
int is_aimee_worktree_path(const char *path);

/* Create a sibling worktree for a git repo. Returns 0 on success. */
int worktree_create_sibling(const char *git_root, const char *session_id);

/* Clean up a session's worktree. Removes if clean, warns if dirty. */
void worktree_cleanup(const char *git_root, const char *session_id);

/* Check if the current branch has a merged PR. Returns 1 if merged. */
int check_merged_pr_for_branch(void);

/* Look up the worktree path for a given CWD from session state.
 * Returns the worktree path if the CWD is inside a tracked git root,
 * or NULL if no worktree applies. */
const char *worktree_for_cwd(const session_state_t *state, const char *cwd);

/* Shared filesystem path validation for all tool paths.
 * Resolves symlinks, rejects traversal attempts, rejects sensitive paths.
 * Returns NULL on success (path is safe), or a static error string on failure.
 * On success, the resolved path is written to resolved_buf. */
const char *guardrails_validate_file_path(const char *path, char *resolved_buf, size_t resolved_len);

#endif /* DEC_GUARDRAILS_H */
