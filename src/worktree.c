/* worktree.c: simplified worktree lifecycle — sibling worktree creation and cleanup.
 *
 * New model: one worktree per git repo per session, created as a sibling
 * directory next to the project root, named <project>-<short-session-id>.
 * No DB registry, no GC system, no complex state tracking. */
#define _GNU_SOURCE
#include "aimee.h"
#include "cJSON.h"
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* Compute the expected sibling worktree path for a git repo and session.
 * For git_root="/root/dev/aimee" and session "abc123...", produces
 * "/root/dev/aimee-abc12345". */
int worktree_sibling_path(const char *git_root, const char *sid,
                          char *wt_buf, size_t wt_len)
{
   if (!git_root || !sid || !wt_buf)
      return -1;

   /* Use first 8 chars of session ID */
   char short_id[12];
   snprintf(short_id, sizeof(short_id), "%.8s", sid);

   snprintf(wt_buf, wt_len, "%s-%s", git_root, short_id);
   return 0;
}

/* Create a sibling worktree. Returns 0 on success, -1 on failure. */
int worktree_create_sibling(const char *git_root, const char *sid)
{
   if (!git_root || !sid)
      return -1;

   char wt_path[MAX_PATH_LEN];
   if (worktree_sibling_path(git_root, sid, wt_path, sizeof(wt_path)) != 0)
      return -1;

   /* Check if worktree already exists and is valid */
   struct stat st;
   if (stat(wt_path, &st) == 0 && S_ISDIR(st.st_mode))
   {
      char git_file[MAX_PATH_LEN];
      snprintf(git_file, sizeof(git_file), "%s/.git", wt_path);
      struct stat git_st;
      if (stat(git_file, &git_st) == 0)
         return 0; /* already exists and valid */

      /* Directory exists but not a git worktree — remove stale dir */
      fprintf(stderr, "aimee: worktree dir '%s' exists but is not a git worktree — removing\n",
              wt_path);
      rmdir(wt_path);
   }

   /* Detect base branch */
   char base_branch[64] = "HEAD";
   {
      const char *candidates[] = {"main", "origin/main", "HEAD"};
      for (int b = 0; b < 3; b++)
      {
         char cmd[MAX_PATH_LEN + 128];
         snprintf(cmd, sizeof(cmd), "git -C '%s' rev-parse --verify '%s' 2>/dev/null",
                  git_root, candidates[b]);
         int rc;
         char *out = run_cmd(cmd, &rc);
         free(out);
         if (rc == 0)
         {
            snprintf(base_branch, sizeof(base_branch), "%s", candidates[b]);
            break;
         }
      }
   }

   /* Create branch name from session ID */
   char short_id[12];
   snprintf(short_id, sizeof(short_id), "%.8s", sid);
   char branch_name[64];
   snprintf(branch_name, sizeof(branch_name), "aimee/session/%s", short_id);

   /* Create the worktree */
   char cmd[MAX_PATH_LEN * 2 + 256];
   snprintf(cmd, sizeof(cmd),
            "git -C '%s' worktree add '%s' -b '%s' '%s' 2>&1",
            git_root, wt_path, branch_name, base_branch);
   int rc;
   char *out = run_cmd(cmd, &rc);

   if (rc == 0)
   {
      fprintf(stderr, "aimee: created worktree at %s\n", wt_path);
      free(out);
      return 0;
   }

   fprintf(stderr, "aimee: failed to create worktree at %s: %s\n", wt_path, out ? out : "unknown");
   free(out);
   return -1;
}

/* Clean up a session's worktree. Removes if clean, warns if dirty. */
void worktree_cleanup(const char *git_root, const char *sid)
{
   if (!git_root || !sid)
      return;

   char wt_path[MAX_PATH_LEN];
   if (worktree_sibling_path(git_root, sid, wt_path, sizeof(wt_path)) != 0)
      return;

   struct stat st;
   if (stat(wt_path, &st) != 0)
      return; /* doesn't exist */

   /* Check for uncommitted changes */
   char cmd[MAX_PATH_LEN + 128];
   snprintf(cmd, sizeof(cmd), "git -C '%s' status --porcelain 2>/dev/null", wt_path);
   int rc;
   char *status = run_cmd(cmd, &rc);
   int has_changes = (status && status[0]);
   free(status);

   /* Check for unpushed commits */
   int has_unpushed = 0;
   if (!has_changes)
   {
      snprintf(cmd, sizeof(cmd),
               "git -C '%s' log @{upstream}..HEAD --oneline 2>/dev/null", wt_path);
      char *log = run_cmd(cmd, &rc);
      has_unpushed = (log && log[0]);
      free(log);
   }

   if (has_changes || has_unpushed)
   {
      fprintf(stderr, "aimee: worktree %s has %s — not removing\n", wt_path,
              has_changes ? "uncommitted changes" : "unpushed commits");
      return;
   }

   /* Clean — remove the worktree */
   snprintf(cmd, sizeof(cmd), "git -C '%s' worktree remove '%s' 2>&1", git_root, wt_path);
   char *out = run_cmd(cmd, &rc);
   if (rc == 0)
      fprintf(stderr, "aimee: removed clean worktree %s\n", wt_path);
   else
      fprintf(stderr, "aimee: failed to remove worktree %s: %s\n", wt_path, out ? out : "unknown");
   free(out);
}

/* Check if the current branch has a merged PR. Returns 1 if merged. */
int check_merged_pr_for_branch(void)
{
   int rc;
   char *branch = run_cmd("git rev-parse --abbrev-ref HEAD 2>/dev/null", &rc);
   if (rc != 0 || !branch)
   {
      free(branch);
      return 0;
   }
   char *nl = strchr(branch, '\n');
   if (nl)
      *nl = '\0';

   /* Skip default branches */
   if (strcmp(branch, "main") == 0 || strcmp(branch, "master") == 0)
   {
      free(branch);
      return 0;
   }

   char cmd[512];
   snprintf(cmd, sizeof(cmd),
            "gh pr list --head '%s' --state merged --json number --limit 1 2>/dev/null", branch);
   free(branch);

   char *out = run_cmd(cmd, &rc);
   if (rc != 0 || !out)
   {
      free(out);
      return 0;
   }

   int has_merged = (strstr(out, "\"number\"") != NULL);
   free(out);
   return has_merged;
}

/* Look up the worktree path for a given CWD from session state.
 * Returns the worktree path if the CWD is inside a tracked git root. */
const char *worktree_for_cwd(const session_state_t *state, const char *cwd)
{
   if (!state || !cwd || state->worktree_count == 0)
      return NULL;

   /* Find the most specific (longest) matching git root */
   int best = -1;
   size_t best_len = 0;
   for (int i = 0; i < state->worktree_count; i++)
   {
      size_t rlen = strlen(state->worktrees[i].git_root);
      if (rlen == 0)
         continue;
      if (strncmp(cwd, state->worktrees[i].git_root, rlen) == 0 &&
          (cwd[rlen] == '/' || cwd[rlen] == '\0'))
      {
         if (rlen > best_len)
         {
            best = i;
            best_len = rlen;
         }
      }
   }

   if (best >= 0)
   {
      /* Check if already in the worktree — don't redirect */
      size_t wt_len = strlen(state->worktrees[best].worktree_path);
      if (strncmp(cwd, state->worktrees[best].worktree_path, wt_len) == 0 &&
          (cwd[wt_len] == '/' || cwd[wt_len] == '\0'))
         return NULL;
      return state->worktrees[best].worktree_path;
   }
   return NULL;
}
