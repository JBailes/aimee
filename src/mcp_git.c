/* mcp_git.c: MCP git tool handlers -- compact git operations for primary agents */
#include "aimee.h"
#include "cJSON.h"
#include "headers/guardrails.h"
#include "headers/git_verify.h"
#include "headers/util.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

#define GIT_BUF_SIZE 65536
#define SUMMARY_MAX  4096

/* Track whether the current MCP git operation is running in a worktree. */
static int s_in_worktree = 0;

void mcp_git_set_worktree(int val)
{
   s_in_worktree = val;
}

int mcp_git_get_worktree(void)
{
   return s_in_worktree;
}

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

static cJSON *mcp_error(const char *fmt, const char *detail)
{
   char buf[1024];
   snprintf(buf, sizeof(buf), fmt, detail);
   return mcp_text(buf);
}

/* --- Merged-PR detection --- */

/* Check if the current branch has a merged PR. Returns 1 if merged, 0 if not.
 * Writes the branch name to branch_buf if provided. */
static int check_branch_has_merged_pr(char *branch_buf, size_t branch_len)
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
   if (branch_buf)
      snprintf(branch_buf, branch_len, "%s", branch);

   /* Skip check for default branches */
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
      return 0; /* gh not available or failed -- don't block */
   }

   /* If output contains a number field, there's a merged PR */
   int has_merged = (strstr(out, "\"number\"") != NULL);
   free(out);
   return has_merged;
}

/* --- Branch ownership --- */

/* Get the git repo root for the current cwd. Returns 0 on success. */
static int get_repo_path(char *buf, size_t len)
{
   int rc;
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

/* Register branch ownership for the current session. Returns 0 on success. */
static int branch_own_register(const char *branch)
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

/* Delete branch ownership record. */
static void branch_own_delete(const char *branch)
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

/* Check if the current session can write to a branch.
 * Returns 1 if allowed, 0 if blocked (owner_out filled with owning session ID). */
static int branch_own_check(const char *branch, char *owner_out, size_t owner_len)
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

/* --- git_status --- */

cJSON *handle_git_status(cJSON *args)
{
   (void)args;
   int rc;
   char *raw = run_cmd("git status --porcelain=v2 --branch 2>&1", &rc);
   if (!raw)
      return mcp_text("error: failed to run git status");

   if (rc != 0)
   {
      cJSON *r = mcp_error("error: git status failed: %s", raw);
      free(raw);
      return r;
   }

   /* Parse porcelain v2 output */
   char branch[256] = "unknown";
   char upstream[256] = "";
   int ahead = 0, behind = 0;
   int staged = 0, modified = 0, untracked = 0, conflicted = 0;

   /* Collect file lists (abbreviated) */
   char staged_files[2048] = "";
   char modified_files[2048] = "";
   char untracked_files[2048] = "";
   size_t sf_len = 0, mf_len = 0, uf_len = 0;

   char *line = raw;
   while (line && *line)
   {
      char *nl = strchr(line, '\n');
      if (nl)
         *nl = '\0';

      if (strncmp(line, "# branch.head ", 14) == 0)
         snprintf(branch, sizeof(branch), "%s", line + 14);
      else if (strncmp(line, "# branch.upstream ", 18) == 0)
         snprintf(upstream, sizeof(upstream), "%s", line + 18);
      else if (strncmp(line, "# branch.ab ", 12) == 0)
         sscanf(line + 12, "+%d -%d", &ahead, &behind);
      else if (line[0] == '1' || line[0] == '2')
      {
         /* Changed entry: XY field at position 2-3 */
         if (strlen(line) > 3)
         {
            char x = line[2], y = line[3];
            /* Extract filename: after the last tab for renamed, or field 9+ */
            const char *fname = line;
            /* Skip 8 space-separated fields to get filename */
            int spaces = 0;
            for (const char *p = line; *p && spaces < 8; p++)
            {
               if (*p == ' ')
                  spaces++;
               if (spaces == 8)
               {
                  fname = p + 1;
                  break;
               }
            }
            /* For renames (type 2), take the part after the tab */
            if (line[0] == '2')
            {
               const char *tab = strchr(fname, '\t');
               if (tab)
                  fname = tab + 1;
            }

            if (x != '.' && x != '?')
            {
               staged++;
               if (sf_len < sizeof(staged_files) - 200)
                  sf_len += (size_t)snprintf(staged_files + sf_len, sizeof(staged_files) - sf_len,
                                             "%s%s", sf_len ? ", " : "", fname);
            }
            if (y != '.' && y != '?')
            {
               modified++;
               if (mf_len < sizeof(modified_files) - 200)
                  mf_len +=
                      (size_t)snprintf(modified_files + mf_len, sizeof(modified_files) - mf_len,
                                       "%s%s", mf_len ? ", " : "", fname);
            }
         }
      }
      else if (line[0] == 'u')
      {
         conflicted++;
      }
      else if (line[0] == '?')
      {
         untracked++;
         const char *fname = line + 2;
         if (uf_len < sizeof(untracked_files) - 200)
            uf_len += (size_t)snprintf(untracked_files + uf_len, sizeof(untracked_files) - uf_len,
                                       "%s%s", uf_len ? ", " : "", fname);
      }

      line = nl ? nl + 1 : NULL;
   }
   free(raw);

   /* Build compact output */
   char out[SUMMARY_MAX];
   int pos = 0;

   pos += snprintf(out + pos, sizeof(out) - (size_t)pos, "branch: %s", branch);
   if (upstream[0])
   {
      if (ahead || behind)
         pos += snprintf(out + pos, sizeof(out) - (size_t)pos, " (ahead %d, behind %d)", ahead,
                         behind);
      else
         pos += snprintf(out + pos, sizeof(out) - (size_t)pos, " (up to date)");
   }
   else
   {
      pos += snprintf(out + pos, sizeof(out) - (size_t)pos, " (no upstream)");
   }

   if (staged)
      pos += snprintf(out + pos, sizeof(out) - (size_t)pos, "\nstaged: %d file%s (%s)", staged,
                      staged == 1 ? "" : "s", staged_files);
   if (modified)
      pos += snprintf(out + pos, sizeof(out) - (size_t)pos, "\nmodified: %d file%s (%s)", modified,
                      modified == 1 ? "" : "s", modified_files);
   if (untracked)
      pos += snprintf(out + pos, sizeof(out) - (size_t)pos, "\nuntracked: %d file%s (%s)",
                      untracked, untracked == 1 ? "" : "s", untracked_files);
   if (conflicted)
      pos += snprintf(out + pos, sizeof(out) - (size_t)pos, "\nconflicted: %d file%s", conflicted,
                      conflicted == 1 ? "" : "s");

   if (!staged && !modified && !untracked && !conflicted)
      pos += snprintf(out + pos, sizeof(out) - (size_t)pos, "\nclean working tree");

   (void)pos;
   return mcp_text(out);
}

/* --- git_commit --- */

cJSON *handle_git_commit(cJSON *args)
{
   cJSON *jmsg = cJSON_GetObjectItemCaseSensitive(args, "message");
   if (!cJSON_IsString(jmsg) || !jmsg->valuestring[0])
      return mcp_text("error: 'message' parameter is required");

   cJSON *jfiles = cJSON_GetObjectItemCaseSensitive(args, "files");

   /* Stage files */
   if (jfiles && cJSON_IsArray(jfiles) && cJSON_GetArraySize(jfiles) > 0)
   {
      /* Stage specific files, skipping sensitive ones */
      char add_cmd[8192] = "git add --";
      size_t cmd_len = strlen(add_cmd);
      int skipped = 0;
      int count = cJSON_GetArraySize(jfiles);

      for (int i = 0; i < count; i++)
      {
         cJSON *f = cJSON_GetArrayItem(jfiles, i);
         if (!cJSON_IsString(f))
            continue;
         if (is_sensitive_file(f->valuestring))
         {
            skipped++;
            continue;
         }
         char *esc = shell_escape(f->valuestring);
         cmd_len += (size_t)snprintf(add_cmd + cmd_len, sizeof(add_cmd) - cmd_len, " '%s'", esc);
         free(esc);
      }

      if (cmd_len > strlen("git add --"))
      {
         strncat(add_cmd, " 2>&1", sizeof(add_cmd) - strlen(add_cmd) - 1);
         int rc;
         char *out = run_cmd(add_cmd, &rc);
         if (rc != 0)
         {
            cJSON *r = mcp_error("error: git add failed: %s", out ? out : "unknown");
            free(out);
            return r;
         }
         free(out);
      }

      if (skipped)
      {
         char warn[256];
         snprintf(warn, sizeof(warn),
                  "warning: skipped %d sensitive file%s (.env, credentials, keys)", skipped,
                  skipped == 1 ? "" : "s");
         /* Continue with commit, but prepend warning */
      }
   }
   else
   {
      /* Stage all modified tracked files (not untracked) */
      int rc;
      char *out = run_cmd("git add -u 2>&1", &rc);
      if (rc != 0)
      {
         cJSON *r = mcp_error("error: git add -u failed: %s", out ? out : "unknown");
         free(out);
         return r;
      }
      free(out);
   }

   /* Commit */
   char *esc_msg = shell_escape(jmsg->valuestring);
   char commit_cmd[8192];
   snprintf(commit_cmd, sizeof(commit_cmd), "git commit -m '%s' 2>&1", esc_msg);
   free(esc_msg);

   int rc;
   char *out = run_cmd(commit_cmd, &rc);
   if (rc != 0)
   {
      cJSON *r = mcp_error("error: git commit failed: %s", out ? out : "unknown");
      free(out);
      return r;
   }
   free(out);

   /* Get commit hash and diffstat */
   char *hash_out = run_cmd("git log -1 --format='%h' 2>&1", &rc);
   char *stat_out = run_cmd("git diff --stat HEAD~1 HEAD 2>&1", &rc);

   char result[SUMMARY_MAX];
   int pos = 0;

   /* Extract short hash */
   char hash[16] = "unknown";
   if (hash_out)
   {
      char *nl = strchr(hash_out, '\n');
      if (nl)
         *nl = '\0';
      snprintf(hash, sizeof(hash), "%s", hash_out);
      free(hash_out);
   }

   pos += snprintf(result + pos, sizeof(result) - (size_t)pos, "committed: %s \"%s\"", hash,
                   jmsg->valuestring);

   /* Extract just the summary line from diffstat */
   if (stat_out)
   {
      /* Last non-empty line is the summary */
      char *last = NULL;
      char *l = stat_out;
      while (l && *l)
      {
         char *nl = strchr(l, '\n');
         if (nl)
            *nl = '\0';
         if (*l)
            last = l;
         l = nl ? nl + 1 : NULL;
      }
      if (last)
         pos += snprintf(result + pos, sizeof(result) - (size_t)pos, "\n%s", last);
      free(stat_out);
   }

   (void)pos;
   return mcp_text(result);
}

/* --- git_push --- */

cJSON *handle_git_push(cJSON *args)
{
   /* Merged-PR enforcement: block pushes to branches with merged PRs */
   {
      char branch[256];
      if (check_branch_has_merged_pr(branch, sizeof(branch)))
         return mcp_text("error: branch has a merged PR. Do not push to merged branches. "
                         "Create a new branch for new work.");
   }

   /* Verify gate: require verification before pushing */
   {
      char verify_msg[256];
      if (!verify_check(NULL, verify_msg, sizeof(verify_msg)))
      {
         char buf[512];
         snprintf(buf, sizeof(buf), "error: verification required before push. %s", verify_msg);
         return mcp_text(buf);
      }
   }

   cJSON *jforce = cJSON_GetObjectItemCaseSensitive(args, "force");
   int force = (jforce && cJSON_IsTrue(jforce)) ? 1 : 0;

   /* Mirror push: replaces all remote refs with local refs */
   cJSON *jmirror = cJSON_GetObjectItemCaseSensitive(args, "mirror");
   if (jmirror && cJSON_IsTrue(jmirror))
   {
      int rc;
      char *out = run_cmd("git push origin --mirror 2>&1", &rc);
      if (rc != 0)
      {
         cJSON *r = mcp_error("error: git push --mirror failed: %s", out ? out : "unknown");
         free(out);
         return r;
      }
      char result[GIT_BUF_SIZE];
      snprintf(result, sizeof(result),
               "mirror push complete — remote now matches local refs exactly.\n%s", out ? out : "");
      free(out);
      return mcp_text(result);
   }

   /* Get current branch */
   int rc;
   char *branch_out = run_cmd("git rev-parse --abbrev-ref HEAD 2>&1", &rc);
   if (rc != 0 || !branch_out)
   {
      cJSON *r = mcp_text("error: not on a branch");
      free(branch_out);
      return r;
   }
   char *nl = strchr(branch_out, '\n');
   if (nl)
      *nl = '\0';
   char branch[256];
   snprintf(branch, sizeof(branch), "%s", branch_out);
   free(branch_out);

   /* Check if upstream exists and whether it matches the local branch name */
   char *upstream = run_cmd("git rev-parse --abbrev-ref @{upstream} 2>&1", &rc);
   int has_upstream = (rc == 0);
   int upstream_matches = 0;
   if (has_upstream && upstream)
   {
      /* upstream is like "origin/branch-name" — compare suffix after '/' */
      char *slash = strchr(upstream, '/');
      const char *upstream_branch = slash ? slash + 1 : upstream;
      char *up_nl = strchr(upstream_branch, '\n');
      if (up_nl)
         *up_nl = '\0';
      upstream_matches = (strcmp(upstream_branch, branch) == 0);
   }
   free(upstream);

   /* Build push command */
   char cmd[512];
   if (has_upstream && upstream_matches)
   {
      if (force)
         snprintf(cmd, sizeof(cmd), "git push --force-with-lease 2>&1");
      else
         snprintf(cmd, sizeof(cmd), "git push 2>&1");
   }
   else
   {
      /* No upstream or name mismatch (e.g., worktree branch tracking main):
       * push current branch to origin and set upstream */
      snprintf(cmd, sizeof(cmd), "git push -u origin '%s' 2>&1", branch);
   }

   char *out = run_cmd(cmd, &rc);
   if (rc != 0)
   {
      cJSON *r = mcp_error("error: git push failed: %s", out ? out : "unknown");
      free(out);
      return r;
   }
   free(out);

   /* Get the commit hash we pushed */
   char *hash_out = run_cmd("git rev-parse --short HEAD 2>&1", &rc);
   char hash[16] = "";
   if (hash_out)
   {
      nl = strchr(hash_out, '\n');
      if (nl)
         *nl = '\0';
      snprintf(hash, sizeof(hash), "%s", hash_out);
      free(hash_out);
   }

   char result[512];
   snprintf(result, sizeof(result), "pushed: %s -> origin/%s (%s)%s", branch, branch, hash,
            force ? " (force-with-lease)" : "");
   return mcp_text(result);
}

/* --- git_branch --- */

cJSON *handle_git_branch(cJSON *args)
{
   cJSON *jaction = cJSON_GetObjectItemCaseSensitive(args, "action");
   if (!cJSON_IsString(jaction))
      return mcp_text("error: 'action' parameter is required (create/switch/list/delete/claim)");

   const char *action = jaction->valuestring;
   cJSON *jname = cJSON_GetObjectItemCaseSensitive(args, "name");

   if (strcmp(action, "list") == 0)
   {
      int rc;
      char *out = run_cmd("git branch -v --format='%(if)%(HEAD)%(then)* %(else)  "
                          "%(end)%(refname:short) (%(objectname:short))' 2>&1",
                          &rc);
      if (rc != 0)
      {
         cJSON *r = mcp_error("error: git branch failed: %s", out ? out : "unknown");
         free(out);
         return r;
      }
      /* Truncate to reasonable length */
      if (out && strlen(out) > 2048)
      {
         const char *suffix = "\n... (truncated)";
         size_t slen = strlen(suffix);
         memcpy(out + 2048, suffix, slen + 1);
      }
      cJSON *r = mcp_text(out ? out : "(no branches)");
      free(out);
      return r;
   }

   if (!cJSON_IsString(jname) || !jname->valuestring[0])
      return mcp_text("error: 'name' parameter is required for create/switch/delete");

   char *esc_name = shell_escape(jname->valuestring);

   if (strcmp(action, "create") == 0)
   {
      cJSON *jbase = cJSON_GetObjectItemCaseSensitive(args, "base");
      char cmd[1024];
      if (cJSON_IsString(jbase) && jbase->valuestring[0])
      {
         char *esc_base = shell_escape(jbase->valuestring);
         snprintf(cmd, sizeof(cmd), "git checkout -b '%s' '%s' 2>&1", esc_name, esc_base);
         free(esc_base);
      }
      else
      {
         snprintf(cmd, sizeof(cmd), "git checkout -b '%s' 2>&1", esc_name);
      }

      int rc;
      char *out = run_cmd(cmd, &rc);
      if (rc != 0)
      {
         cJSON *r = mcp_error("error: git branch create failed: %s", out ? out : "unknown");
         free(out);
         free(esc_name);
         return r;
      }
      free(out);

      /* Get current commit hash */
      char *hash_out = run_cmd("git rev-parse --short HEAD 2>&1", &rc);
      char hash[16] = "";
      if (hash_out)
      {
         char *nl = strchr(hash_out, '\n');
         if (nl)
            *nl = '\0';
         snprintf(hash, sizeof(hash), "%s", hash_out);
         free(hash_out);
      }

      /* Register branch ownership for this session */
      branch_own_register(jname->valuestring);

      char result[512];
      snprintf(result, sizeof(result), "created: %s (%s)\nswitched to %s\nowner: %s",
               jname->valuestring, hash, jname->valuestring, session_id());
      free(esc_name);
      return mcp_text(result);
   }

   if (strcmp(action, "switch") == 0)
   {
      char cmd[512];
      snprintf(cmd, sizeof(cmd), "git checkout '%s' 2>&1", esc_name);
      int rc;
      char *out = run_cmd(cmd, &rc);
      if (rc != 0)
      {
         cJSON *r = mcp_error("error: git switch failed: %s", out ? out : "unknown");
         free(out);
         free(esc_name);
         return r;
      }
      free(out);

      char result[256];
      snprintf(result, sizeof(result), "switched to %s", jname->valuestring);
      free(esc_name);
      return mcp_text(result);
   }

   if (strcmp(action, "orphan") == 0)
   {
      char cmd[512];
      snprintf(cmd, sizeof(cmd), "git checkout --orphan '%s' 2>&1", esc_name);
      int rc;
      char *out = run_cmd(cmd, &rc);
      if (rc != 0)
      {
         cJSON *r = mcp_error("error: git checkout --orphan failed: %s", out ? out : "unknown");
         free(out);
         free(esc_name);
         return r;
      }
      free(out);

      /* Register branch ownership for this session */
      branch_own_register(jname->valuestring);

      char result[512];
      snprintf(result, sizeof(result),
               "created orphan branch: %s\n"
               "All files are staged. Commit to create the root commit.\n"
               "owner: %s",
               jname->valuestring, session_id());
      free(esc_name);
      return mcp_text(result);
   }

   if (strcmp(action, "delete") == 0)
   {
      cJSON *jforce_del = cJSON_GetObjectItemCaseSensitive(args, "force");
      int force_del = (jforce_del && cJSON_IsTrue(jforce_del)) ? 1 : 0;
      cJSON *jremote = cJSON_GetObjectItemCaseSensitive(args, "remote");
      int remote = (jremote && cJSON_IsTrue(jremote)) ? 1 : 0;

      char cmd[512];
      snprintf(cmd, sizeof(cmd), "git branch %s '%s' 2>&1", force_del ? "-D" : "-d", esc_name);
      int rc;
      char *out = run_cmd(cmd, &rc);
      if (rc != 0)
      {
         cJSON *r = mcp_error("error: git branch delete failed: %s", out ? out : "unknown");
         free(out);
         free(esc_name);
         return r;
      }
      free(out);

      /* Also delete remote branch if requested */
      char remote_result[256] = "";
      if (remote)
      {
         char rcmd[512];
         snprintf(rcmd, sizeof(rcmd), "git push origin --delete '%s' 2>&1", esc_name);
         char *rout = run_cmd(rcmd, &rc);
         if (rc != 0)
            snprintf(remote_result, sizeof(remote_result), "\nremote delete failed: %s",
                     rout ? rout : "unknown");
         else
            snprintf(remote_result, sizeof(remote_result), "\nremote branch deleted");
         free(rout);
      }

      /* Clean up ownership record */
      branch_own_delete(jname->valuestring);

      char result[512];
      snprintf(result, sizeof(result), "deleted: %s%s", jname->valuestring, remote_result);
      free(esc_name);
      return mcp_text(result);
   }

   if (strcmp(action, "claim") == 0)
   {
      if (strcmp(jname->valuestring, "main") == 0 || strcmp(jname->valuestring, "master") == 0)
      {
         free(esc_name);
         return mcp_text("error: cannot claim main/master branches");
      }
      char owner[64];
      if (!branch_own_check(jname->valuestring, owner, sizeof(owner)))
      {
         char err[512];
         snprintf(err, sizeof(err),
                  "error: branch '%s' is owned by session %.20s. "
                  "Cannot claim a branch owned by another session.",
                  jname->valuestring, owner);
         free(esc_name);
         return mcp_text(err);
      }
      if (branch_own_register(jname->valuestring) != 0)
      {
         free(esc_name);
         return mcp_text("error: failed to register branch ownership");
      }
      char result[256];
      snprintf(result, sizeof(result), "claimed: %s (owner: %s)", jname->valuestring, session_id());
      free(esc_name);
      return mcp_text(result);
   }

   free(esc_name);
   return mcp_text("error: unknown action. Use create/switch/list/delete/claim");
}

/* --- git_log --- */

cJSON *handle_git_log(cJSON *args)
{
   cJSON *jcount = cJSON_GetObjectItemCaseSensitive(args, "count");
   cJSON *jref = cJSON_GetObjectItemCaseSensitive(args, "ref");
   cJSON *jstat = cJSON_GetObjectItemCaseSensitive(args, "diff_stat");

   int count = 10;
   if (cJSON_IsNumber(jcount) && jcount->valueint > 0 && jcount->valueint <= 50)
      count = jcount->valueint;

   const char *stat_flag = (jstat && cJSON_IsTrue(jstat)) ? " --stat" : "";

   char cmd[1024];
   if (cJSON_IsString(jref) && jref->valuestring[0])
   {
      char *esc = shell_escape(jref->valuestring);
      snprintf(cmd, sizeof(cmd), "git log --format='%%h %%ar  %%s'%s -n %d '%s' 2>&1", stat_flag,
               count, esc);
      free(esc);
   }
   else
   {
      snprintf(cmd, sizeof(cmd), "git log --format='%%h %%ar  %%s'%s -n %d 2>&1", stat_flag, count);
   }

   int rc;
   char *out = run_cmd(cmd, &rc);
   if (rc != 0)
   {
      cJSON *r = mcp_error("error: git log failed: %s", out ? out : "unknown");
      free(out);
      return r;
   }

   /* Truncate if very long */
   if (out && strlen(out) > 4000)
   {
      out[4000] = '\0';
      size_t len = strlen(out);
      snprintf(out + len, GIT_BUF_SIZE - len, "\n... (truncated)");
   }

   cJSON *r = mcp_text(out && out[0] ? out : "(no commits)");
   free(out);
   return r;
}

/* --- git_diff_summary --- */

cJSON *handle_git_diff_summary(cJSON *args)
{
   cJSON *jref = cJSON_GetObjectItemCaseSensitive(args, "ref");
   cJSON *jstat = cJSON_GetObjectItemCaseSensitive(args, "stat_only");
   cJSON *jfiles = cJSON_GetObjectItemCaseSensitive(args, "files");

   int stat_only = 1; /* default true */
   if (jstat && cJSON_IsFalse(jstat))
      stat_only = 0;

   /* Build command */
   char cmd[4096];
   size_t cmd_len = 0;

   if (stat_only)
   {
      if (cJSON_IsString(jref) && jref->valuestring[0])
      {
         char *esc = shell_escape(jref->valuestring);
         cmd_len = (size_t)snprintf(cmd, sizeof(cmd), "git diff --stat '%s'", esc);
         free(esc);
      }
      else
      {
         cmd_len = (size_t)snprintf(cmd, sizeof(cmd), "git diff --stat");
      }
   }
   else
   {
      /* Full diff but we'll compress it */
      if (cJSON_IsString(jref) && jref->valuestring[0])
      {
         char *esc = shell_escape(jref->valuestring);
         cmd_len = (size_t)snprintf(cmd, sizeof(cmd), "git diff '%s'", esc);
         free(esc);
      }
      else
      {
         cmd_len = (size_t)snprintf(cmd, sizeof(cmd), "git diff");
      }
   }

   /* Append file filters */
   if (jfiles && cJSON_IsArray(jfiles) && cJSON_GetArraySize(jfiles) > 0)
   {
      cmd_len += (size_t)snprintf(cmd + cmd_len, sizeof(cmd) - cmd_len, " --");
      int fcount = cJSON_GetArraySize(jfiles);
      for (int i = 0; i < fcount && i < 20; i++)
      {
         cJSON *f = cJSON_GetArrayItem(jfiles, i);
         if (cJSON_IsString(f))
         {
            char *esc = shell_escape(f->valuestring);
            cmd_len += (size_t)snprintf(cmd + cmd_len, sizeof(cmd) - cmd_len, " '%s'", esc);
            free(esc);
         }
      }
   }

   snprintf(cmd + cmd_len, sizeof(cmd) - cmd_len, " 2>&1");

   int rc;
   char *out = run_cmd(cmd, &rc);
   if (rc != 0)
   {
      cJSON *r = mcp_error("error: git diff failed: %s", out ? out : "unknown");
      free(out);
      return r;
   }

   if (!out || !out[0])
   {
      free(out);
      return mcp_text("no changes");
   }

   if (stat_only)
   {
      /* Already compact, just truncate if needed */
      if (strlen(out) > 3000)
      {
         out[3000] = '\0';
         size_t len = strlen(out);
         snprintf(out + len, GIT_BUF_SIZE - len, "\n... (truncated)");
      }
      cJSON *r = mcp_text(out);
      free(out);
      return r;
   }

   /* Compress full diff to change descriptions per file.
    * Include hunk headers and changed lines (truncated per file) so the
    * caller can understand *what* changed, not just how many lines. */
   char summary[SUMMARY_MAX];
   int spos = 0;
   char current_file[512] = "";
   int file_adds = 0, file_dels = 0;
   int file_count = 0;
   int file_lines = 0;                       /* changed lines emitted for current file */
   static const int MAX_LINES_PER_FILE = 40; /* cap per file to keep summary compact */

   char *line = out;
   while (line && *line && spos < SUMMARY_MAX - 200)
   {
      char *nl = strchr(line, '\n');
      if (nl)
         *nl = '\0';

      if (strncmp(line, "diff --git ", 11) == 0)
      {
         /* Flush previous file header */
         if (current_file[0] && file_count <= 20)
         {
            if (file_lines >= MAX_LINES_PER_FILE)
               spos += snprintf(summary + spos, sizeof(summary) - (size_t)spos,
                                "  ... (%d more changed lines)\n",
                                (file_adds + file_dels) - file_lines);
         }
         /* Parse new file: "diff --git a/foo b/foo" -> "foo" */
         const char *b = strstr(line, " b/");
         if (b)
            snprintf(current_file, sizeof(current_file), "%s", b + 3);
         else
            snprintf(current_file, sizeof(current_file), "%s", line + 11);
         file_adds = 0;
         file_dels = 0;
         file_lines = 0;
         file_count++;
         if (file_count <= 20)
            spos += snprintf(summary + spos, sizeof(summary) - (size_t)spos, "%s\n", current_file);
      }
      else if (strncmp(line, "@@ ", 3) == 0 && file_count <= 20)
      {
         /* Hunk header — include it for context */
         spos += snprintf(summary + spos, sizeof(summary) - (size_t)spos, "  %s\n", line);
         file_lines = 0; /* reset per-hunk counter so each hunk gets some lines */
      }
      else if ((line[0] == '+' && line[1] != '+') || (line[0] == '-' && line[1] != '-'))
      {
         if (line[0] == '+')
            file_adds++;
         else
            file_dels++;
         if (file_count <= 20 && file_lines < MAX_LINES_PER_FILE)
         {
            /* Truncate very long lines */
            if (strlen(line) > 120)
               spos +=
                   snprintf(summary + spos, sizeof(summary) - (size_t)spos, "  %.120s...\n", line);
            else
               spos += snprintf(summary + spos, sizeof(summary) - (size_t)spos, "  %s\n", line);
            file_lines++;
         }
      }

      line = nl ? nl + 1 : NULL;
   }

   /* Flush last file overflow */
   if (current_file[0] && file_count <= 20 && file_lines >= MAX_LINES_PER_FILE)
   {
      spos += snprintf(summary + spos, sizeof(summary) - (size_t)spos,
                       "  ... (%d more changed lines)\n", (file_adds + file_dels) - file_lines);
   }

   if (file_count > 20)
      spos += snprintf(summary + spos, sizeof(summary) - (size_t)spos, "... and %d more files\n",
                       file_count - 20);

   (void)spos;
   free(out);
   return mcp_text(summary[0] ? summary : "no changes");
}

/* --- git_pr --- */

cJSON *handle_git_pr(cJSON *args)
{
   cJSON *jaction = cJSON_GetObjectItemCaseSensitive(args, "action");
   if (!cJSON_IsString(jaction))
      return mcp_text("error: 'action' parameter is required (create/view/list/merge_status)");

   const char *action = jaction->valuestring;

   if (strcmp(action, "merge_status") == 0)
   {
      cJSON *jnum = cJSON_GetObjectItemCaseSensitive(args, "number");
      if (!cJSON_IsNumber(jnum))
         return mcp_text("error: 'number' parameter is required for merge_status");

      char cmd[256];
      snprintf(cmd, sizeof(cmd),
               "gh pr view %d --json state,mergedAt,title --template "
               "'PR #%d: {{.state}}{{if .mergedAt}} (merged {{.mergedAt}}){{end}} - "
               "{{.title}}' 2>&1",
               jnum->valueint, jnum->valueint);

      int rc;
      char *out = run_cmd(cmd, &rc);
      if (rc != 0)
      {
         cJSON *r = mcp_error("error: gh pr view failed: %s", out ? out : "unknown");
         free(out);
         return r;
      }
      cJSON *r = mcp_text(out ? out : "unknown");
      free(out);
      return r;
   }

   if (strcmp(action, "view") == 0)
   {
      cJSON *jnum = cJSON_GetObjectItemCaseSensitive(args, "number");
      if (!cJSON_IsNumber(jnum))
         return mcp_text("error: 'number' parameter is required for view");

      char cmd[256];
      snprintf(cmd, sizeof(cmd),
               "gh pr view %d --json title,state,url,baseRefName,headRefName,mergedAt "
               "--template 'PR #%d: {{.state}}\\ntitle: {{.title}}\\n"
               "base: {{.baseRefName}} <- {{.headRefName}}\\nurl: {{.url}}"
               "{{if .mergedAt}}\\nmerged: {{.mergedAt}}{{end}}' 2>&1",
               jnum->valueint, jnum->valueint);

      int rc;
      char *out = run_cmd(cmd, &rc);
      if (rc != 0)
      {
         cJSON *r = mcp_error("error: gh pr view failed: %s", out ? out : "unknown");
         free(out);
         return r;
      }
      cJSON *r = mcp_text(out ? out : "unknown");
      free(out);
      return r;
   }

   if (strcmp(action, "list") == 0)
   {
      int rc;
      char *out = run_cmd(
          "gh pr list --limit 20 --json number,title,state,headRefName "
          "--template '{{range .}}#{{.number}} [{{.state}}] {{.headRefName}}: {{.title}}\n{{end}}' "
          "2>&1",
          &rc);
      if (rc != 0)
      {
         cJSON *r = mcp_error("error: gh pr list failed: %s", out ? out : "unknown");
         free(out);
         return r;
      }
      cJSON *r = mcp_text(out && out[0] ? out : "(no open PRs)");
      free(out);
      return r;
   }

   if (strcmp(action, "create") == 0)
   {
      /* Merged-PR enforcement: block creating PRs from branches with merged PRs */
      {
         char branch[256];
         if (check_branch_has_merged_pr(branch, sizeof(branch)))
            return mcp_text("error: branch already has a merged PR. "
                            "Create a new branch for new work.");
      }

      /* Verify gate: require verification before creating PR */
      {
         char verify_msg[256];
         if (!verify_check(NULL, verify_msg, sizeof(verify_msg)))
         {
            char buf[512];
            snprintf(buf, sizeof(buf), "error: verification required before creating PR. %s",
                     verify_msg);
            return mcp_text(buf);
         }
      }

      cJSON *jtitle = cJSON_GetObjectItemCaseSensitive(args, "title");
      cJSON *jbody = cJSON_GetObjectItemCaseSensitive(args, "body");
      cJSON *jbase = cJSON_GetObjectItemCaseSensitive(args, "base");

      if (!cJSON_IsString(jtitle) || !jtitle->valuestring[0])
         return mcp_text("error: 'title' parameter is required for create");

      char *esc_title = shell_escape(jtitle->valuestring);
      char *esc_body = shell_escape(cJSON_IsString(jbody) ? jbody->valuestring : "");
      const char *base = cJSON_IsString(jbase) ? jbase->valuestring : "main";
      char *esc_base = shell_escape(base);

      char cmd[8192];
      snprintf(cmd, sizeof(cmd), "gh pr create --title '%s' --body '%s' --base '%s' 2>&1",
               esc_title, esc_body, esc_base);
      free(esc_title);
      free(esc_body);
      free(esc_base);

      int rc;
      char *out = run_cmd(cmd, &rc);
      if (rc != 0)
      {
         cJSON *r = mcp_error("error: gh pr create failed: %s", out ? out : "unknown");
         free(out);
         return r;
      }

      /* Output from gh pr create is typically just the URL */
      char result[1024];
      if (out)
      {
         /* Trim trailing newline */
         size_t len = strlen(out);
         while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r'))
            out[--len] = '\0';
         snprintf(result, sizeof(result), "created: \"%s\"\nurl: %s\nbase: %s", jtitle->valuestring,
                  out, base);
      }
      else
      {
         snprintf(result, sizeof(result), "created: \"%s\" (base: %s)", jtitle->valuestring, base);
      }
      free(out);
      return mcp_text(result);
   }

   return mcp_text("error: unknown action. Use create/view/list/merge_status");
}

/* --- git_pull --- */

cJSON *handle_git_pull(cJSON *args)
{
   cJSON *jrebase = cJSON_GetObjectItemCaseSensitive(args, "rebase");
   int rebase = (jrebase && cJSON_IsTrue(jrebase)) ? 1 : 0;

   int rc;
   char *branch_out = run_cmd("git rev-parse --abbrev-ref HEAD 2>&1", &rc);
   char branch[256] = "";
   if (branch_out)
   {
      char *nl = strchr(branch_out, '\n');
      if (nl)
         *nl = '\0';
      snprintf(branch, sizeof(branch), "%s", branch_out);
      free(branch_out);
   }

   /* Get pre-pull HEAD for summary */
   char *pre_hash = run_cmd("git rev-parse --short HEAD 2>/dev/null", &rc);
   char pre[16] = "";
   if (pre_hash)
   {
      char *nl = strchr(pre_hash, '\n');
      if (nl)
         *nl = '\0';
      snprintf(pre, sizeof(pre), "%s", pre_hash);
      free(pre_hash);
   }

   char cmd[512];
   if (rebase)
      snprintf(cmd, sizeof(cmd), "git pull --rebase 2>&1");
   else
      snprintf(cmd, sizeof(cmd), "git pull 2>&1");

   char *out = run_cmd(cmd, &rc);
   if (rc != 0)
   {
      cJSON *r = mcp_error("error: git pull failed: %s", out ? out : "unknown");
      free(out);
      return r;
   }

   /* Get post-pull HEAD */
   char *post_hash = run_cmd("git rev-parse --short HEAD 2>/dev/null", &rc);
   char post[16] = "";
   if (post_hash)
   {
      char *nl = strchr(post_hash, '\n');
      if (nl)
         *nl = '\0';
      snprintf(post, sizeof(post), "%s", post_hash);
      free(post_hash);
   }

   char result[512];
   if (strcmp(pre, post) == 0)
      snprintf(result, sizeof(result), "already up to date on %s (%s)", branch, post);
   else
      snprintf(result, sizeof(result), "pulled: %s %s..%s%s", branch, pre, post,
               rebase ? " (rebased)" : "");

   free(out);
   return mcp_text(result);
}

/* --- git_clone --- */

cJSON *handle_git_clone(cJSON *args)
{
   cJSON *jurl = cJSON_GetObjectItemCaseSensitive(args, "url");
   if (!cJSON_IsString(jurl) || !jurl->valuestring[0])
      return mcp_text("error: 'url' parameter is required");

   cJSON *jpath = cJSON_GetObjectItemCaseSensitive(args, "path");
   cJSON *jbranch = cJSON_GetObjectItemCaseSensitive(args, "branch");
   cJSON *jdepth = cJSON_GetObjectItemCaseSensitive(args, "depth");

   char *esc_url = shell_escape(jurl->valuestring);

   char cmd[2048];
   int pos = snprintf(cmd, sizeof(cmd), "git clone");

   if (cJSON_IsString(jbranch) && jbranch->valuestring[0])
   {
      char *esc = shell_escape(jbranch->valuestring);
      pos += snprintf(cmd + pos, sizeof(cmd) - (size_t)pos, " -b '%s'", esc);
      free(esc);
   }
   if (cJSON_IsNumber(jdepth) && jdepth->valueint > 0)
      pos += snprintf(cmd + pos, sizeof(cmd) - (size_t)pos, " --depth %d", jdepth->valueint);

   pos += snprintf(cmd + pos, sizeof(cmd) - (size_t)pos, " '%s'", esc_url);

   if (cJSON_IsString(jpath) && jpath->valuestring[0])
   {
      char *esc = shell_escape(jpath->valuestring);
      pos += snprintf(cmd + pos, sizeof(cmd) - (size_t)pos, " '%s'", esc);
      free(esc);
   }

   snprintf(cmd + pos, sizeof(cmd) - (size_t)pos, " 2>&1");
   free(esc_url);

   int rc;
   char *out = run_cmd(cmd, &rc);
   if (rc != 0)
   {
      cJSON *r = mcp_error("error: git clone failed: %s", out ? out : "unknown");
      free(out);
      return r;
   }

   char result[512];
   snprintf(result, sizeof(result), "cloned: %s", jurl->valuestring);
   free(out);
   return mcp_text(result);
}

/* --- git_stash --- */

cJSON *handle_git_stash(cJSON *args)
{
   cJSON *jaction = cJSON_GetObjectItemCaseSensitive(args, "action");
   const char *action =
       (cJSON_IsString(jaction) && jaction->valuestring[0]) ? jaction->valuestring : "push";

   int rc;
   char cmd[1024];

   if (strcmp(action, "push") == 0)
   {
      cJSON *jmsg = cJSON_GetObjectItemCaseSensitive(args, "message");
      if (cJSON_IsString(jmsg) && jmsg->valuestring[0])
      {
         char *esc = shell_escape(jmsg->valuestring);
         snprintf(cmd, sizeof(cmd), "git stash push -m '%s' 2>&1", esc);
         free(esc);
      }
      else
      {
         snprintf(cmd, sizeof(cmd), "git stash push 2>&1");
      }
   }
   else if (strcmp(action, "pop") == 0)
   {
      snprintf(cmd, sizeof(cmd), "git stash pop 2>&1");
   }
   else if (strcmp(action, "apply") == 0)
   {
      cJSON *jindex = cJSON_GetObjectItemCaseSensitive(args, "index");
      int idx = (cJSON_IsNumber(jindex)) ? jindex->valueint : 0;
      snprintf(cmd, sizeof(cmd), "git stash apply stash@{%d} 2>&1", idx);
   }
   else if (strcmp(action, "list") == 0)
   {
      snprintf(cmd, sizeof(cmd), "git stash list 2>&1");
   }
   else if (strcmp(action, "drop") == 0)
   {
      cJSON *jindex = cJSON_GetObjectItemCaseSensitive(args, "index");
      int idx = (cJSON_IsNumber(jindex)) ? jindex->valueint : 0;
      snprintf(cmd, sizeof(cmd), "git stash drop stash@{%d} 2>&1", idx);
   }
   else
   {
      return mcp_text("error: unknown action. Use push/pop/apply/list/drop");
   }

   char *out = run_cmd(cmd, &rc);
   if (rc != 0)
   {
      cJSON *r = mcp_error("error: git stash %s failed: %s", out ? out : "unknown");
      free(out);
      return r;
   }

   cJSON *r = mcp_text(out && out[0] ? out : "(no output)");
   free(out);
   return r;
}

/* --- git_tag --- */

cJSON *handle_git_tag(cJSON *args)
{
   cJSON *jaction = cJSON_GetObjectItemCaseSensitive(args, "action");
   const char *action =
       (cJSON_IsString(jaction) && jaction->valuestring[0]) ? jaction->valuestring : "list";

   int rc;
   char cmd[1024];

   if (strcmp(action, "list") == 0)
   {
      snprintf(cmd, sizeof(cmd), "git tag --sort=-creatordate -n1 2>&1");

      char *out = run_cmd(cmd, &rc);
      if (rc != 0)
      {
         cJSON *r = mcp_error("error: git tag list failed: %s", out ? out : "unknown");
         free(out);
         return r;
      }
      cJSON *r = mcp_text(out && out[0] ? out : "(no tags)");
      free(out);
      return r;
   }

   cJSON *jname = cJSON_GetObjectItemCaseSensitive(args, "name");
   if (!cJSON_IsString(jname) || !jname->valuestring[0])
      return mcp_text("error: 'name' parameter is required for create/delete");

   char *esc_name = shell_escape(jname->valuestring);

   if (strcmp(action, "create") == 0)
   {
      cJSON *jmsg = cJSON_GetObjectItemCaseSensitive(args, "message");
      cJSON *jref = cJSON_GetObjectItemCaseSensitive(args, "ref");

      if (cJSON_IsString(jmsg) && jmsg->valuestring[0])
      {
         char *esc_msg = shell_escape(jmsg->valuestring);
         if (cJSON_IsString(jref) && jref->valuestring[0])
         {
            char *esc_ref = shell_escape(jref->valuestring);
            snprintf(cmd, sizeof(cmd), "git tag -a '%s' -m '%s' '%s' 2>&1", esc_name, esc_msg,
                     esc_ref);
            free(esc_ref);
         }
         else
         {
            snprintf(cmd, sizeof(cmd), "git tag -a '%s' -m '%s' 2>&1", esc_name, esc_msg);
         }
         free(esc_msg);
      }
      else
      {
         if (cJSON_IsString(jref) && jref->valuestring[0])
         {
            char *esc_ref = shell_escape(jref->valuestring);
            snprintf(cmd, sizeof(cmd), "git tag '%s' '%s' 2>&1", esc_name, esc_ref);
            free(esc_ref);
         }
         else
         {
            snprintf(cmd, sizeof(cmd), "git tag '%s' 2>&1", esc_name);
         }
      }
   }
   else if (strcmp(action, "delete") == 0)
   {
      snprintf(cmd, sizeof(cmd), "git tag -d '%s' 2>&1", esc_name);
   }
   else
   {
      free(esc_name);
      return mcp_text("error: unknown action. Use create/list/delete");
   }

   free(esc_name);

   char *out = run_cmd(cmd, &rc);
   if (rc != 0)
   {
      cJSON *r = mcp_error("error: git tag %s failed: %s", out ? out : "unknown");
      free(out);
      return r;
   }

   char result[512];
   if (strcmp(action, "create") == 0)
      snprintf(result, sizeof(result), "tagged: %s", jname->valuestring);
   else
      snprintf(result, sizeof(result), "deleted tag: %s", jname->valuestring);

   free(out);
   return mcp_text(result);
}

/* --- git_fetch --- */

cJSON *handle_git_fetch(cJSON *args)
{
   cJSON *jprune = cJSON_GetObjectItemCaseSensitive(args, "prune");
   int prune = (jprune && cJSON_IsTrue(jprune)) ? 1 : 0;

   cJSON *jremote = cJSON_GetObjectItemCaseSensitive(args, "remote");
   const char *remote =
       (cJSON_IsString(jremote) && jremote->valuestring[0]) ? jremote->valuestring : "origin";

   char *esc_remote = shell_escape(remote);
   char cmd[512];
   if (prune)
      snprintf(cmd, sizeof(cmd), "git fetch --prune '%s' 2>&1", esc_remote);
   else
      snprintf(cmd, sizeof(cmd), "git fetch '%s' 2>&1", esc_remote);
   free(esc_remote);

   int rc;
   char *out = run_cmd(cmd, &rc);
   if (rc != 0)
   {
      cJSON *r = mcp_error("error: git fetch failed: %s", out ? out : "unknown");
      free(out);
      return r;
   }

   char result[512];
   snprintf(result, sizeof(result), "fetched from %s%s", remote, prune ? " (pruned)" : "");
   free(out);
   return mcp_text(result);
}

/* --- git_reset --- */

cJSON *handle_git_reset(cJSON *args)
{
   cJSON *jref = cJSON_GetObjectItemCaseSensitive(args, "ref");
   const char *ref = (cJSON_IsString(jref) && jref->valuestring[0]) ? jref->valuestring : "HEAD~1";

   cJSON *jmode = cJSON_GetObjectItemCaseSensitive(args, "mode");
   const char *mode =
       (cJSON_IsString(jmode) && jmode->valuestring[0]) ? jmode->valuestring : "mixed";

   /* Validate mode */
   if (strcmp(mode, "soft") != 0 && strcmp(mode, "mixed") != 0 && strcmp(mode, "hard") != 0)
      return mcp_text("error: mode must be soft, mixed, or hard");

   char *esc_ref = shell_escape(ref);
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "git reset --%s '%s' 2>&1", mode, esc_ref);
   free(esc_ref);

   int rc;
   char *out = run_cmd(cmd, &rc);
   if (rc != 0)
   {
      cJSON *r = mcp_error("error: git reset failed: %s", out ? out : "unknown");
      free(out);
      return r;
   }

   /* Get new HEAD */
   char *hash_out = run_cmd("git rev-parse --short HEAD 2>/dev/null", &rc);
   char hash[16] = "";
   if (hash_out)
   {
      char *nl = strchr(hash_out, '\n');
      if (nl)
         *nl = '\0';
      snprintf(hash, sizeof(hash), "%s", hash_out);
      free(hash_out);
   }

   char result[512];
   snprintf(result, sizeof(result), "reset (--%s) to %s, HEAD now at %s", mode, ref, hash);
   free(out);
   return mcp_text(result);
}

/* --- git_restore --- */

cJSON *handle_git_restore(cJSON *args)
{
   cJSON *jfiles = cJSON_GetObjectItemCaseSensitive(args, "files");
   cJSON *jstaged = cJSON_GetObjectItemCaseSensitive(args, "staged");
   cJSON *jsource = cJSON_GetObjectItemCaseSensitive(args, "source");
   int staged = (jstaged && cJSON_IsTrue(jstaged)) ? 1 : 0;

   if (!cJSON_IsArray(jfiles) || cJSON_GetArraySize(jfiles) == 0)
      return mcp_text("error: 'files' parameter is required (array of file paths)");

   /* Build file list */
   char file_args[4096] = "";
   int fpos = 0;
   int file_count = cJSON_GetArraySize(jfiles);
   for (int i = 0; i < file_count && i < 50; i++)
   {
      cJSON *f = cJSON_GetArrayItem(jfiles, i);
      if (!cJSON_IsString(f))
         continue;
      char *esc = shell_escape(f->valuestring);
      fpos += snprintf(file_args + fpos, sizeof(file_args) - (size_t)fpos, " '%s'", esc);
      free(esc);
   }

   char cmd[8192];
   int cpos = snprintf(cmd, sizeof(cmd), "git restore");

   if (staged)
      cpos += snprintf(cmd + cpos, sizeof(cmd) - (size_t)cpos, " --staged");

   if (cJSON_IsString(jsource) && jsource->valuestring[0])
   {
      char *esc = shell_escape(jsource->valuestring);
      cpos += snprintf(cmd + cpos, sizeof(cmd) - (size_t)cpos, " --source='%s'", esc);
      free(esc);
   }

   snprintf(cmd + cpos, sizeof(cmd) - (size_t)cpos, "%s 2>&1", file_args);

   int rc;
   char *out = run_cmd(cmd, &rc);
   if (rc != 0)
   {
      cJSON *r = mcp_error("error: git restore failed: %s", out ? out : "unknown");
      free(out);
      return r;
   }

   char result[512];
   snprintf(result, sizeof(result), "restored %d file(s)%s", file_count,
            staged ? " (unstaged)" : "");
   free(out);
   return mcp_text(result);
}

/* --- CWD helper for git tools --- */

int mcp_chdir_git_root(char *old_cwd, size_t old_cwd_len, cJSON *args)
{
   if (!getcwd(old_cwd, old_cwd_len))
      return 0;

   /* Priority 1: explicit 'path' argument from tool call */
   if (args)
   {
      cJSON *jpath = cJSON_GetObjectItemCaseSensitive(args, "path");
      if (cJSON_IsString(jpath) && jpath->valuestring[0])
      {
         if (chdir(jpath->valuestring) == 0)
            return 1;
      }
   }

   /* Priority 2: session CWD tracking file */
   {
      char cwd_path[MAX_PATH_LEN];
      snprintf(cwd_path, sizeof(cwd_path), "%s/git-cwd-%s", config_output_dir(), session_id());
      FILE *fp = fopen(cwd_path, "r");
      if (fp)
      {
         char tracked[MAX_PATH_LEN] = "";
         if (fgets(tracked, sizeof(tracked), fp))
         {
            size_t len = strlen(tracked);
            while (len > 0 && (tracked[len - 1] == '\n' || tracked[len - 1] == '\r'))
               tracked[--len] = '\0';
            if (tracked[0] && chdir(tracked) == 0)
            {
               fclose(fp);
               return 1;
            }
         }
         fclose(fp);
      }
   }

   /* Priority 3: CWD is already a git repo root */
   if (access(".git", F_OK) == 0)
      return 0;

   /* Priority 3b: CWD is inside a git repo -- walk up to the root */
   {
      int rc;
      char *toplevel = run_cmd("git rev-parse --show-toplevel 2>/dev/null", &rc);
      if (rc == 0 && toplevel && toplevel[0])
      {
         /* Strip trailing newline */
         size_t len = strlen(toplevel);
         while (len > 0 && (toplevel[len - 1] == '\n' || toplevel[len - 1] == '\r'))
            toplevel[--len] = '\0';
         if (len > 0 && chdir(toplevel) == 0)
         {
            free(toplevel);
            return 1;
         }
      }
      free(toplevel);
   }

   /* Priority 4: subdirectory with .git + .aimee/project.yaml */
   DIR *d = opendir(".");
   if (!d)
      return 0;

   struct dirent *ent;
   while ((ent = readdir(d)) != NULL)
   {
      if (ent->d_name[0] == '.')
         continue;
      if (ent->d_type != DT_DIR && ent->d_type != DT_UNKNOWN)
         continue;

      char probe[MAX_PATH_LEN];
      snprintf(probe, sizeof(probe), "%s/.git", ent->d_name);
      if (access(probe, F_OK) != 0)
         continue;
      snprintf(probe, sizeof(probe), "%s/.aimee/project.yaml", ent->d_name);
      if (access(probe, F_OK) != 0)
         continue;

      if (chdir(ent->d_name) == 0)
      {
         closedir(d);
         return 1;
      }
   }
   closedir(d);
   return 0;
}
