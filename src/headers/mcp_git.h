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

/* Change to git root directory before running git tools.
 * Saves old cwd in old_cwd. Returns 1 if chdir happened, 0 otherwise.
 * Caller should restore old_cwd when done. */
int mcp_chdir_git_root(char *old_cwd, size_t old_cwd_len, cJSON *args);

/* Worktree context: set by dispatch when operating inside a session worktree.
 * Handlers use this to avoid changing the worktree's checked-out branch. */
void mcp_git_set_worktree(int in_worktree);
int mcp_git_in_worktree(void);

#endif /* DEC_MCP_GIT_H */
