/* branch_ownership.c -- track which session owns each git branch.
 *
 * Prevents cross-session clobbering by blocking commit/push/PR/reset
 * on branches owned by other sessions. main/master are always shared. */
#include "aimee.h"
#include "cJSON.h"
#include "headers/branch_ownership.h"
#include <string.h>

/* --- Helpers --- */

static cJSON *mcp_text(const char *text)
{
   cJSON *arr = cJSON_CreateArray();
   cJSON *item = cJSON_CreateObject();
   cJSON_AddStringToObject(item, "type", "text");
   cJSON_AddStringToObject(item, "text", text);
   cJSON_AddItemToArray(arr, item);
   return arr;
}

static int get_current_branch(char *buf, size_t len)
{
   int rc;
   char *out = run_cmd("git rev-parse --abbrev-ref HEAD 2>/dev/null", &rc);
   if (rc != 0 || !out)
   {
      free(out);
      return -1;
   }
   char *nl = strchr(out, '\n');
   if (nl)
      *nl = '\0';
   snprintf(buf, len, "%s", out);
   free(out);
   return 0;
}

/* --- Repo path resolution --- */

/* Get the canonical git repo root for the current cwd. Returns 0 on success.
 * In a worktree, --show-toplevel returns the worktree root, not the main repo.
 * We use --git-common-dir to find the shared .git dir and derive the main root,
 * so ownership records are consistent across main checkout and worktrees. */
int get_repo_path(char *buf, size_t len)
{
   int rc;

   /* Try --git-common-dir first: in a worktree this returns the absolute path
    * to the main repo's .git dir (e.g. "/root/dev/aimee/.git").
    * In a regular checkout it returns ".git" (relative). */
   char *common = run_cmd("git rev-parse --git-common-dir 2>/dev/null", &rc);
   if (rc == 0 && common)
   {
      char *nl = strchr(common, '\n');
      if (nl)
         *nl = '\0';

      if (strcmp(common, ".git") != 0 && common[0] == '/')
      {
         /* Absolute path like "/root/dev/aimee/.git" or
          * "/root/dev/aimee/.git/worktrees/..." — strip from /.git onward */
         char *git_suffix = strstr(common, "/.git");
         if (git_suffix)
         {
            *git_suffix = '\0';
            snprintf(buf, len, "%s", common);
            free(common);
            return 0;
         }
      }
      free(common);
   }
   else
   {
      free(common);
   }

   /* Fallback: regular checkout — use --show-toplevel */
   char *out = run_cmd("git rev-parse --show-toplevel 2>/dev/null", &rc);
   if (rc != 0 || !out)
   {
      free(out);
      return -1;
   }
   char *nl = strchr(out, '\n');
   if (nl)
      *nl = '\0';
   snprintf(buf, len, "%s", out);
   free(out);
   return 0;
}

/* --- Ownership operations --- */

int branch_own_register(const char *branch)
{
   sqlite3 *db = mcp_db_get();
   if (!db)
      return -1;
   char repo[MAX_PATH_LEN];
   if (get_repo_path(repo, sizeof(repo)) != 0)
      return -1;

   sqlite3_stmt *st = db_prepare(
       db, "INSERT OR REPLACE INTO branch_ownership (repo_path, branch_name, session_id) "
           "VALUES (?, ?, ?)");
   if (!st)
      return -1;
   sqlite3_bind_text(st, 1, repo, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, branch, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 3, session_id(), -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   return (rc == SQLITE_DONE) ? 0 : -1;
}

void branch_own_delete(const char *branch)
{
   sqlite3 *db = mcp_db_get();
   if (!db)
      return;
   char repo[MAX_PATH_LEN];
   if (get_repo_path(repo, sizeof(repo)) != 0)
      return;

   sqlite3_stmt *st =
       db_prepare(db, "DELETE FROM branch_ownership WHERE repo_path = ? AND branch_name = ?");
   if (!st)
      return;
   sqlite3_bind_text(st, 1, repo, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, branch, -1, SQLITE_TRANSIENT);
   sqlite3_step(st);
}

int branch_own_check(const char *branch, char *owner_out, size_t owner_len)
{
   /* main/master are shared — never owned */
   if (strcmp(branch, "main") == 0 || strcmp(branch, "master") == 0)
      return 1;

   sqlite3 *db = mcp_db_get();
   if (!db)
      return 1; /* no db = no enforcement */

   char repo[MAX_PATH_LEN];
   if (get_repo_path(repo, sizeof(repo)) != 0)
      return 1;

   sqlite3_stmt *st = db_prepare(
       db, "SELECT session_id FROM branch_ownership WHERE repo_path = ? AND branch_name = ?");
   if (!st)
      return 1;
   sqlite3_bind_text(st, 1, repo, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, branch, -1, SQLITE_TRANSIENT);

   if (sqlite3_step(st) == SQLITE_ROW)
   {
      const char *owner = (const char *)sqlite3_column_text(st, 0);
      if (strcmp(owner, session_id()) != 0)
      {
         snprintf(owner_out, owner_len, "%s", owner);
         return 0; /* blocked */
      }
   }
   /* No owner recorded or owned by current session = allowed */
   return 1;
}

int mcp_git_branch_own_register(const char *repo_path, const char *branch)
{
   sqlite3 *db = mcp_db_get();
   if (!db)
      return -1;

   sqlite3_stmt *st = db_prepare(
       db, "INSERT OR REPLACE INTO branch_ownership (repo_path, branch_name, session_id) "
           "VALUES (?, ?, ?)");
   if (!st)
      return -1;
   sqlite3_bind_text(st, 1, repo_path, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, branch, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 3, session_id(), -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   return (rc == SQLITE_DONE) ? 0 : -1;
}

cJSON *branch_own_guard(const char *operation)
{
   char branch[256];
   if (get_current_branch(branch, sizeof(branch)) != 0)
      return NULL; /* can't determine branch — allow */

   char owner[64];
   if (!branch_own_check(branch, owner, sizeof(owner)))
   {
      char buf[512];
      snprintf(buf, sizeof(buf),
               "error: %s blocked — branch '%s' is owned by session %.20s. "
               "Use git_branch action=claim to take ownership.",
               operation, branch, owner);
      return mcp_text(buf);
   }
   return NULL; /* allowed */
}
