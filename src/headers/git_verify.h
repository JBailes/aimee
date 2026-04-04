#ifndef DEC_GIT_VERIFY_H
#define DEC_GIT_VERIFY_H 1

#include "cJSON.h"

#define MAX_VERIFY_STEPS 16
#define MAX_STEP_NAME    64
#define MAX_STEP_CMD     512
#define VERIFY_TTL_SECS  3600

typedef struct
{
   char name[MAX_STEP_NAME];
   char run[MAX_STEP_CMD];
} verify_step_t;

typedef struct
{
   verify_step_t steps[MAX_VERIFY_STEPS];
   int count;
} verify_config_t;

/* Load verify steps from .aimee/project.yaml. Returns 0 on success, -1 if no
 * verify section found. project_root may be NULL (uses cwd). */
int verify_load_config(const char *project_root, verify_config_t *cfg);

/* Run all verify steps. Returns 0 if all pass, -1 if any fail.
 * Writes per-step results to stderr. On full success, records file-mtime
 * hash and timestamp to .aimee/.last-verify. */
int verify_run_all(const char *project_root, verify_config_t *cfg);

/* Check whether the last verification is still valid.
 * Valid means: file-mtime hash unchanged AND within TTL (3600s).
 * Writes explanatory message to msg_buf. If no verify section in project.yaml,
 * returns 1 (no gate). Returns 1 if valid, 0 if verification required. */
int verify_check(const char *project_root, char *msg_buf, size_t msg_len);

/* Compute a hash of all tracked file paths + their mtimes.
 * Returns a hex string (caller must free), or NULL on failure. */
char *verify_compute_file_hash(const char *project_root);

/* MCP tool handler: git_verify. */
cJSON *handle_git_verify(cJSON *args);

#endif /* DEC_GIT_VERIFY_H */
