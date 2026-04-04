#ifndef DEC_GIT_VERIFY_H
#define DEC_GIT_VERIFY_H 1

#include "cJSON.h"

#define MAX_VERIFY_STEPS 16
#define MAX_STEP_NAME    64
#define MAX_STEP_CMD     512

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
 * Writes per-step results to stderr. On full success, records HEAD hash
 * to .aimee/.last-verify. */
int verify_run_all(const char *project_root, verify_config_t *cfg);

/* Check whether current HEAD matches the last recorded successful verification.
 * Writes explanatory message to msg_buf. If no verify section in project.yaml,
 * returns 1. */
int verify_check_head(const char *project_root, char *msg_buf, size_t msg_len);

/* MCP tool handler: git_verify. */
cJSON *handle_git_verify(cJSON *args);

#endif /* DEC_GIT_VERIFY_H */
