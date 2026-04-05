#ifndef DEC_MCP_GIT_H
#define DEC_MCP_GIT_H 1

#include "cJSON.h"

/* MCP git tool handlers. Each returns a cJSON array of content blocks
 * (text type) suitable for MCP tools/call responses. */

cJSON *handle_git_status(cJSON *args);
cJSON *handle_git_commit(cJSON *args);
cJSON *handle_git_push(cJSON *args);
cJSON *handle_git_branch(cJSON *args);
cJSON *handle_git_log(cJSON *args);
cJSON *handle_git_diff_summary(cJSON *args);
cJSON *handle_git_pr(cJSON *args);
cJSON *handle_git_verify(cJSON *args);
cJSON *handle_git_pull(cJSON *args);
cJSON *handle_git_clone(cJSON *args);
cJSON *handle_git_stash(cJSON *args);
cJSON *handle_git_tag(cJSON *args);
cJSON *handle_git_fetch(cJSON *args);
cJSON *handle_git_reset(cJSON *args);
cJSON *handle_git_restore(cJSON *args);

/* Track whether the current MCP git operation is running in a worktree. */
void mcp_git_set_worktree(int val);
int mcp_git_get_worktree(void);

/* Register branch ownership for the current session in the given repo.
 * Used by worktree creation to track branches it creates. */
int mcp_git_branch_own_register(const char *repo_path, const char *branch);

/* Change to git root directory before running git tools.
 * Saves old cwd in old_cwd. Returns 1 if chdir happened, 0 otherwise.
 * Caller should restore old_cwd when done. */
int mcp_chdir_git_root(char *old_cwd, size_t old_cwd_len, cJSON *args);

#endif /* DEC_MCP_GIT_H */
