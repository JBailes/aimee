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

typedef struct
{
   char name[128];
   char path[MAX_PATH_LEN];
   char workspace_root[MAX_PATH_LEN]; /* original repo path for lazy creation */
   char base_branch[64];              /* cached base branch (e.g. "main", "origin/main") */
   int created;                       /* 0=pending, 1=created, -1=failed */
} worktree_entry_t;

typedef struct
{
   char seen_paths[MAX_SEEN_PATHS][MAX_SEEN_LEN];
   int seen_count;
   char session_mode[16];
   char guardrail_mode[16];
   int64_t active_task_id;
   worktree_entry_t worktrees[MAX_WORKTREES];
   int worktree_count;
   uint16_t fetched_mask; /* bitmask: which worktrees have had git fetch this session */
   char prev_main_head[MAX_WORKTREES][64]; /* previous session's main HEAD per workspace */
   int hook_call_count; /* increments each pre_tool_check call for diagnostics */
   int dirty;

   /* Parallel startup: worktree readiness gate.
    * 0=pending, 1=ready, -1=failed. Write operations block until ready. */
   atomic_int worktree_ready;
   pthread_mutex_t wt_mutex;
   pthread_cond_t wt_cond;
} session_state_t;

/* Thread argument for background worktree creation */
typedef struct
{
   session_state_t *state;
   int ws_index; /* which worktree entry to create */
   int result;   /* 0=success, -1=failure */
} worktree_thread_arg_t;

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

/* Check if a command is any git/gh operation (read or write). */
int is_git_command(const char *command);

/* Check if a tool is a shell/bash provider. */
int is_shell_tool(const char *tool);

/* Normalize a relative path against cwd. Result written to buf. */
char *normalize_path(const char *path, const char *cwd, char *buf, size_t buf_len);

/* Ensure a worktree entry has been created. Returns 0 on success, -1 on failure.
 * No-op if already created. Sets created flag to 1 on success, -1 on failure. */
int worktree_ensure(worktree_entry_t *entry);

/* Resolve a worktree path by name, lazily creating on first access.
 * Returns the worktree path, or NULL on failure. */
const char *worktree_resolve_path(session_state_t *state, const char *name);

/* Look up the worktree path for a given workspace root. Returns the worktree
 * path if the normalized file path falls inside a workspace that has a
 * worktree, or NULL if no worktree applies. Ensures the worktree is created. */
const char *worktree_for_path(session_state_t *state, const config_t *cfg, const char *norm_path);

/* Register a worktree creation in the database (best-effort). */
void worktree_db_register(const char *sid, const char *workspace, const char *path);

/* Update last_accessed_at for a worktree (best-effort). */
void worktree_db_touch(const char *path);

/* Run worktree garbage collection. Removes stale worktrees, respects disk budget.
 * Returns number of worktrees cleaned. */
int worktree_gc(sqlite3 *db, const config_t *cfg, int64_t disk_budget_bytes, int verbose);

/* Same as worktree_for_path but only returns the path if the worktree has
 * already been created. Does NOT trigger creation. Used by read guards so
 * reads can go to the original repo before the first write. */
const char *worktree_for_path_if_created(session_state_t *state, const config_t *cfg,
                                         const char *norm_path);

/* Shared filesystem path validation for all tool paths.
 * Resolves symlinks, rejects traversal attempts, rejects sensitive paths.
 * Returns NULL on success (path is safe), or a static error string on failure.
 * On success, the resolved path is written to resolved_buf. */
const char *guardrails_validate_file_path(const char *path, char *resolved_buf, size_t resolved_len);

/* Initialize the worktree readiness gate (mutex + condvar). */
void worktree_gate_init(session_state_t *state);

/* Signal that all worktrees are ready (or failed). Called after threads join. */
void worktree_gate_signal(session_state_t *state, int ready);

/* Wait for worktree readiness. Returns 1 if ready, -1 if failed.
 * Used by pre_tool_check to gate write operations. */
int worktree_gate_wait(session_state_t *state);

/* Background worktree creation thread function. */
void *worktree_thread_fn(void *arg);

#endif /* DEC_GUARDRAILS_H */
