#ifndef DEC_BRANCH_OWNERSHIP_H
#define DEC_BRANCH_OWNERSHIP_H 1

#include "cJSON.h"

/* Get the canonical git repo root (resolves through worktrees). */
int get_repo_path(char *buf, size_t len);

/* Register branch ownership for the current session. Returns 0 on success. */
int branch_own_register(const char *branch);

/* Delete branch ownership record. */
void branch_own_delete(const char *branch);

/* Check if the current session can write to a branch.
 * Returns 1 if allowed, 0 if blocked (owner_out filled with owning session ID). */
int branch_own_check(const char *branch, char *owner_out, size_t owner_len);

/* Register branch ownership with an explicit repo path.
 * Used by worktree creation which creates branches outside the normal flow. */
int mcp_git_branch_own_register(const char *repo_path, const char *branch);

/* Check branch ownership for the current branch. Returns NULL if allowed,
 * or an error cJSON response if blocked by another session's ownership. */
cJSON *branch_own_guard(const char *operation);

#endif /* DEC_BRANCH_OWNERSHIP_H */
