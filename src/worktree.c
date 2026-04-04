/* worktree.c: worktree lifecycle — creation, resolution, GC, and readiness gate. */
#define _GNU_SOURCE
#include "aimee.h"
#include "cJSON.h"
#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* Register a worktree in the database (best-effort). */
void worktree_db_register(const char *sid, const char *workspace, const char *path)
{
   config_t cfg;
   config_load(&cfg);
   sqlite3 *db = db_open_fast(cfg.db_path);
   if (!db)
      return;

   time_t now = time(NULL);
   sqlite3_stmt *stmt = db_prepare(db, "INSERT OR IGNORE INTO worktrees(session_id, workspace, "
                                       "path, created_at, last_accessed_at, state)"
                                       " VALUES(?, ?, ?, ?, ?, 'active')");
   if (stmt)
   {
      sqlite3_bind_text(stmt, 1, sid, -1, SQLITE_STATIC);
      sqlite3_bind_text(stmt, 2, workspace, -1, SQLITE_STATIC);
      sqlite3_bind_text(stmt, 3, path, -1, SQLITE_STATIC);
      sqlite3_bind_int64(stmt, 4, (int64_t)now);
      sqlite3_bind_int64(stmt, 5, (int64_t)now);
      DB_STEP_LOG(stmt, "worktree_db_register");
   }
   db_close(db);
}

/* Update last_accessed_at for a worktree (best-effort). */
void worktree_db_touch(const char *path)
{
   config_t cfg;
   config_load(&cfg);
   sqlite3 *db = db_open_fast(cfg.db_path);
   if (!db)
      return;

   time_t now = time(NULL);
   sqlite3_stmt *stmt = db_prepare(
       db, "UPDATE worktrees SET last_accessed_at = ? WHERE path = ? AND state = 'active'");
   if (stmt)
   {
      sqlite3_bind_int64(stmt, 1, (int64_t)now);
      sqlite3_bind_text(stmt, 2, path, -1, SQLITE_STATIC);
      DB_STEP_LOG(stmt, "worktree_db_touch");
   }
   db_close(db);
}

/* Ensure a worktree entry has been created on disk. No-op if already created. */
int worktree_ensure(worktree_entry_t *entry)
{
   if (!entry)
      return -1;
   if (entry->created == 1)
      return 0;
   if (entry->created == -1)
      return -1; /* already failed, don't retry */

   /* Check if worktree directory already exists AND is a valid git worktree.
    * A valid git worktree has a .git file (not directory) that points back to
    * the main repo's .git/worktrees/ entry. A plain directory without .git is
    * NOT a valid worktree — it may have been left behind by a failed creation
    * or created by an unrelated process. */
   struct stat st;
   if (stat(entry->path, &st) == 0 && S_ISDIR(st.st_mode))
   {
      char git_file[MAX_PATH_LEN];
      snprintf(git_file, sizeof(git_file), "%s/.git", entry->path);
      struct stat git_st;
      if (stat(git_file, &git_st) == 0)
      {
         entry->created = 1;
         return 0;
      }
      /* Directory exists but is not a git worktree — remove it so git worktree
       * add can succeed (it refuses to create into a non-empty directory). */
      fprintf(stderr,
              "aimee: worktree dir '%s' exists but is not a git worktree — removing stale dir\n",
              entry->path);
      rmdir(entry->path);
   }

   /* Need workspace_root to create */
   if (!entry->workspace_root[0])
   {
      fprintf(stderr, "aimee: cannot create worktree '%s': no workspace_root\n", entry->name);
      entry->created = -1;
      return -1;
   }

   /* Validate workspace_root exists on disk */
   struct stat ws_st;
   if (stat(entry->workspace_root, &ws_st) != 0 || !S_ISDIR(ws_st.st_mode))
   {
      fprintf(stderr, "aimee: cannot create worktree '%s': workspace_root '%s' does not exist\n",
              entry->name, entry->workspace_root);
      entry->created = -1;
      return -1;
   }

   /* Fire-and-forget background fetch so refs are updated for next time.
    * Double-fork to avoid zombies: child forks grandchild, child exits
    * immediately, grandchild does the actual fetch. */
   {
      pid_t fetch_pid = fork();
      if (fetch_pid == 0)
      {
         /* Child: fork grandchild, then exit immediately */
         if (fork() == 0)
         {
            /* Grandchild: detach and run fetch */
            setsid();
            const char *fargv[] = {"git",    "-C", entry->workspace_root, "fetch", "--quiet",
                                   "origin", NULL};
            char *fout = NULL;
            safe_exec_capture(fargv, &fout, 256);
            free(fout);
            _exit(0);
         }
         _exit(0);
      }
      if (fetch_pid > 0)
         waitpid(fetch_pid, NULL, 0); /* reap the intermediate child immediately */
   }

   /* Create parent directory */
   char wt_parent[MAX_PATH_LEN];
   snprintf(wt_parent, sizeof(wt_parent), "%s", entry->path);
   char *slash = strrchr(wt_parent, '/');
   if (slash)
   {
      *slash = '\0';
      mkdir(wt_parent, 0700);
   }

   /* Extract session ID for branch name from the path.
    * Path format: .../worktrees/<session-id>/<project-name> */
   const char *sid_start = NULL;
   const char *wt_str = strstr(entry->path, "/worktrees/");
   if (wt_str)
   {
      sid_start = wt_str + strlen("/worktrees/");
      /* sid_start points to "session-id/project-name" */
   }

   char short_id[12] = {0};
   if (sid_start)
   {
      /* Take first 8 chars of session ID */
      const char *sid_end = strchr(sid_start, '/');
      int sid_len = sid_end ? (int)(sid_end - sid_start) : (int)strlen(sid_start);
      if (sid_len > 8)
         sid_len = 8;
      snprintf(short_id, sizeof(short_id), "%.*s", sid_len, sid_start);
   }
   else
   {
      snprintf(short_id, sizeof(short_id), "unknown");
   }

   char branch_name[64];
   snprintf(branch_name, sizeof(branch_name), "aimee/session/%s", short_id);

   /* Detect the correct base branch via git rev-parse (fast, local only).
    * Cache the result in entry->base_branch to avoid re-detection. */
   if (!entry->base_branch[0])
   {
      const char *candidates[] = {"main", "origin/main", "HEAD"};
      for (int b = 0; b < 3; b++)
      {
         char *out = NULL;
         const char *rp_argv[] = {
             "git", "-C", entry->workspace_root, "rev-parse", "--verify", candidates[b], NULL};
         if (safe_exec_capture(rp_argv, &out, 256) == 0)
         {
            snprintf(entry->base_branch, sizeof(entry->base_branch), "%s", candidates[b]);
            free(out);
            break;
         }
         free(out);
      }
      if (!entry->base_branch[0])
         snprintf(entry->base_branch, sizeof(entry->base_branch), "HEAD");
   }

   /* Create the worktree using the detected base branch */
   int rc = -1;
   {
      char *out = NULL;
      const char *argv[] = {"git",       "-C", entry->workspace_root, "worktree",         "add",
                            entry->path, "-b", branch_name,           entry->base_branch, NULL};
      rc = safe_exec_capture(argv, &out, 1024);
      free(out);
   }

   if (rc == 0)
   {
      entry->created = 1;
      fprintf(stderr, "aimee: created worktree '%s' at %s\n", entry->name, entry->path);

      /* Register in worktree registry (best-effort, non-fatal) */
      worktree_db_register(session_id(), entry->workspace_root, entry->path);

      return 0;
   }

   fprintf(stderr, "aimee: failed to create worktree '%s'\n", entry->name);
   entry->created = -1;
   return -1;
}

/* Resolve a worktree by name, lazily creating it on first access. */
const char *worktree_resolve_path(session_state_t *state, const char *name)
{
   if (!state || !name)
      return NULL;

   for (int i = 0; i < state->worktree_count; i++)
   {
      if (strcmp(state->worktrees[i].name, name) == 0)
      {
         if (worktree_ensure(&state->worktrees[i]) == 0)
         {
            worktree_db_touch(state->worktrees[i].path);
            state->dirty = 1;
            return state->worktrees[i].path;
         }
         return NULL;
      }
   }
   return NULL;
}

const char *worktree_for_path(session_state_t *state, const config_t *cfg, const char *norm_path)
{
   if (!state || !cfg || !norm_path || state->worktree_count == 0)
      return NULL;

   /* Find the most specific (longest) matching workspace to avoid parent
    * directories like /root/dev matching before /root/dev/aimee. */
   int best = -1;
   size_t best_len = 0;
   for (int i = 0; i < cfg->workspace_count; i++)
   {
      size_t wlen = strlen(cfg->workspaces[i]);
      if (wlen == 0)
         continue;
      if (strncmp(norm_path, cfg->workspaces[i], wlen) == 0 &&
          (norm_path[wlen] == '/' || norm_path[wlen] == '\0'))
      {
         if (wlen > best_len)
         {
            best = i;
            best_len = wlen;
         }
      }
   }
   if (best >= 0)
   {
      /* If the path is already inside a .claude/worktrees/ subdirectory of the
       * matched workspace, the agent is working in a Claude Code worktree and
       * should not be redirected to an aimee-managed worktree. */
      const char *remainder = norm_path + best_len;
      if (remainder[0] == '/')
         remainder++;
      if (strncmp(remainder, ".claude/worktrees/", 18) == 0)
         return NULL;

      const char *slash = strrchr(cfg->workspaces[best], '/');
      const char *ws_name = slash ? slash + 1 : cfg->workspaces[best];
      const char *resolved = worktree_resolve_path(state, ws_name);
      /* Verify the resolved path actually exists on disk. worktree_ensure may
       * report success for a stale entry or the path may have been removed
       * between sessions. Redirecting to a nonexistent path is worse than
       * allowing the operation. */
      if (resolved)
      {
         struct stat st;
         if (stat(resolved, &st) != 0 || !S_ISDIR(st.st_mode))
            return NULL;
      }
      return resolved;
   }
   return NULL;
}

/* Return the worktree path for a workspace only if it has already been created.
 * Does NOT trigger worktree creation. Used by read guards so that reads can
 * go to the original repo when the worktree hasn't been created yet. */
const char *worktree_for_path_if_created(session_state_t *state, const config_t *cfg,
                                         const char *norm_path)
{
   if (!state || !cfg || !norm_path || state->worktree_count == 0)
      return NULL;

   /* Find the most specific (longest) matching workspace. */
   int best = -1;
   size_t best_len = 0;
   for (int i = 0; i < cfg->workspace_count; i++)
   {
      size_t wlen = strlen(cfg->workspaces[i]);
      if (wlen == 0)
         continue;
      if (strncmp(norm_path, cfg->workspaces[i], wlen) == 0 &&
          (norm_path[wlen] == '/' || norm_path[wlen] == '\0'))
      {
         if (wlen > best_len)
         {
            best = i;
            best_len = wlen;
         }
      }
   }
   if (best >= 0)
   {
      /* If the path is already inside a .claude/worktrees/ subdirectory of the
       * matched workspace, the agent is working in a Claude Code worktree and
       * should not be redirected to an aimee-managed worktree. */
      const char *remainder = norm_path + best_len;
      if (remainder[0] == '/')
         remainder++;
      if (strncmp(remainder, ".claude/worktrees/", 18) == 0)
         return NULL;

      const char *slash = strrchr(cfg->workspaces[best], '/');
      const char *ws_name = slash ? slash + 1 : cfg->workspaces[best];
      for (int j = 0; j < state->worktree_count; j++)
      {
         if (strcmp(state->worktrees[j].name, ws_name) == 0 && state->worktrees[j].created == 1)
            return state->worktrees[j].path;
      }
   }
   return NULL;
}

/* Compute directory size recursively (bytes). */
static int64_t dir_size_bytes(const char *path)
{
   int64_t total = 0;
   DIR *d = opendir(path);
   if (!d)
      return 0;
   struct dirent *ent;
   while ((ent = readdir(d)) != NULL)
   {
      if (ent->d_name[0] == '.')
         continue;
      char sub[MAX_PATH_LEN];
      snprintf(sub, sizeof(sub), "%s/%s", path, ent->d_name);
      struct stat st;
      if (lstat(sub, &st) != 0)
         continue;
      if (S_ISDIR(st.st_mode))
         total += dir_size_bytes(sub);
      else
         total += st.st_size;
   }
   closedir(d);
   return total;
}

/* Run worktree garbage collection. */
int worktree_gc(sqlite3 *db, const config_t *cfg, int64_t disk_budget_bytes, int verbose)
{
   if (!db || !cfg)
      return 0;

   const char *current_sid = session_id();
   time_t now = time(NULL);
   int cleaned = 0;
   int64_t freed = 0;

   /* Phase 1: Mark worktrees as stale if their session is ended (no state file)
    * and last_accessed_at is older than 24 hours. */
   {
      sqlite3_stmt *stmt =
          db_prepare(db, "SELECT id, session_id, workspace, path, last_accessed_at"
                         " FROM worktrees WHERE state = 'active' ORDER BY last_accessed_at ASC");
      if (!stmt)
         return 0;

      const char *config_dir = config_output_dir();

      /* Collect IDs to mark stale (can't modify while iterating) */
      int64_t stale_ids[256];
      int stale_count = 0;

      while (sqlite3_step(stmt) == SQLITE_ROW && stale_count < 256)
      {
         int64_t id = sqlite3_column_int64(stmt, 0);
         const char *sid = (const char *)sqlite3_column_text(stmt, 1);
         int64_t last_access = sqlite3_column_int64(stmt, 4);

         /* Never clean the current session */
         if (!sid)
            continue;
         if (strcmp(sid, current_sid) == 0)
            continue;

         /* Check if session is still active (state file exists and is recent) */
         char state_file[MAX_PATH_LEN];
         snprintf(state_file, sizeof(state_file), "%s/session-%s.state", config_dir, sid);
         struct stat st;
         if (stat(state_file, &st) == 0 && difftime(now, st.st_mtime) < 86400)
            continue; /* session still active */

         /* Check age threshold (24 hours) */
         if (difftime(now, (time_t)last_access) < 86400)
            continue;

         stale_ids[stale_count++] = id;
      }

      /* Mark stale */
      for (int i = 0; i < stale_count; i++)
      {
         sqlite3_stmt *upd = db_prepare(db, "UPDATE worktrees SET state = 'stale' WHERE id = ?");
         if (upd)
         {
            sqlite3_bind_int64(upd, 1, stale_ids[i]);
            DB_STEP_LOG(upd, "worktree_gc");
         }
      }
   }

   /* Phase 2: Remove stale worktrees from disk. */
   {
      sqlite3_stmt *stmt = db_prepare(db, "SELECT id, session_id, workspace, path FROM worktrees"
                                          " WHERE state = 'stale' ORDER BY last_accessed_at ASC");
      if (!stmt)
         return cleaned;

      while (sqlite3_step(stmt) == SQLITE_ROW)
      {
         int64_t id = sqlite3_column_int64(stmt, 0);
         const char *sid = (const char *)sqlite3_column_text(stmt, 1);
         const char *workspace = (const char *)sqlite3_column_text(stmt, 2);
         const char *path = (const char *)sqlite3_column_text(stmt, 3);

         /* Compute size before removal */
         int64_t sz = dir_size_bytes(path);

         /* Remove via git worktree remove */
         char *exec_out = NULL;
         const char *rm_argv[] = {"git",    "-C",      workspace, "worktree",
                                  "remove", "--force", path,      NULL};
         safe_exec_capture(rm_argv, &exec_out, 1024);
         free(exec_out);

         /* Delete session branch */
         char short_id[12];
         snprintf(short_id, sizeof(short_id), "%.8s", sid ? sid : "");
         char branch_name[64];
         snprintf(branch_name, sizeof(branch_name), "aimee/session/%s", short_id);
         const char *br_argv[] = {"git", "-C", workspace, "branch", "-d", branch_name, NULL};
         exec_out = NULL;
         safe_exec_capture(br_argv, &exec_out, 1024);
         free(exec_out);

         /* Soft-delete in DB */
         sqlite3_stmt *upd =
             db_prepare(db, "UPDATE worktrees SET state = 'deleted', size_bytes = ? WHERE id = ?");
         if (upd)
         {
            sqlite3_bind_int64(upd, 1, sz);
            sqlite3_bind_int64(upd, 2, id);
            DB_STEP_LOG(upd, "guardrails");
         }

         freed += sz;
         cleaned++;

         if (verbose)
            fprintf(stderr, "aimee: gc: removed worktree %s (%.1f MB)\n", path,
                    (double)sz / (1024.0 * 1024.0));
      }
   }

   /* Phase 3: Disk budget enforcement. If total active worktree size exceeds
    * budget, we already cleaned stale ones above. Check if still over. */
   if (disk_budget_bytes > 0)
   {
      /* Sum size of all active worktrees */
      int64_t total_size = 0;
      {
         sqlite3_stmt *stmt = db_prepare(db, "SELECT path FROM worktrees WHERE state = 'active'");
         if (stmt)
         {
            while (sqlite3_step(stmt) == SQLITE_ROW)
            {
               const char *path = (const char *)sqlite3_column_text(stmt, 0);
               total_size += dir_size_bytes(path);
            }
         }
      }

      if (total_size > disk_budget_bytes && verbose)
      {
         fprintf(stderr,
                 "aimee: gc: warning: active worktrees use %.1f MB (budget: %.1f MB)\n"
                 "  Cannot clean active session worktrees. Consider ending idle sessions.\n",
                 (double)total_size / (1024.0 * 1024.0),
                 (double)disk_budget_bytes / (1024.0 * 1024.0));
      }
   }

   /* Phase 4: Clean up session directories that are now empty. */
   {
      char wt_dir[MAX_PATH_LEN];
      snprintf(wt_dir, sizeof(wt_dir), "%s/worktrees", config_output_dir());
      DIR *d = opendir(wt_dir);
      if (d)
      {
         struct dirent *ent;
         while ((ent = readdir(d)) != NULL)
         {
            if (ent->d_name[0] == '.')
               continue;
            if (strcmp(ent->d_name, current_sid) == 0)
               continue;
            char session_dir[MAX_PATH_LEN];
            snprintf(session_dir, sizeof(session_dir), "%s/%s", wt_dir, ent->d_name);
            rmdir(session_dir); /* only succeeds if empty */
         }
         closedir(d);
      }
   }

   if (verbose && cleaned > 0)
      fprintf(stderr, "aimee: gc: cleaned %d worktree(s), freed %.1f MB\n", cleaned,
              (double)freed / (1024.0 * 1024.0));

   return cleaned;
}

/* --- Parallel startup: worktree readiness gate --- */

void worktree_gate_init(session_state_t *state)
{
   atomic_store(&state->worktree_ready, 0);
   pthread_mutex_init(&state->wt_mutex, NULL);
   pthread_cond_init(&state->wt_cond, NULL);
}

void worktree_gate_signal(session_state_t *state, int ready)
{
   pthread_mutex_lock(&state->wt_mutex);
   atomic_store(&state->worktree_ready, ready);
   pthread_cond_broadcast(&state->wt_cond);
   pthread_mutex_unlock(&state->wt_mutex);
}

int worktree_gate_wait(session_state_t *state)
{
   int val = atomic_load(&state->worktree_ready);
   if (val != 0)
      return val;

   pthread_mutex_lock(&state->wt_mutex);
   while ((val = atomic_load(&state->worktree_ready)) == 0)
      pthread_cond_wait(&state->wt_cond, &state->wt_mutex);
   pthread_mutex_unlock(&state->wt_mutex);
   return val;
}

void *worktree_thread_fn(void *arg)
{
   worktree_thread_arg_t *wta = arg;
   wta->result = worktree_ensure(&wta->state->worktrees[wta->ws_index]);
   return NULL;
}
