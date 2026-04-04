/* guardrails.c: path classification, pre-tool safety checks, session state management */
#define _GNU_SOURCE
#include "aimee.h"
#include "cJSON.h"
#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* In-session anti-pattern hit tracking. Process-scoped (resets on session end). */
#define AP_HIT_MAX_PATTERNS    64
#define AP_HIT_BLOCK_THRESHOLD 3

static struct
{
   int64_t pattern_id;
   int hits;
} ap_hit_counts[AP_HIT_MAX_PATTERNS];
static int ap_hit_count = 0;

/* Sensitive filename patterns (substring match) */
static const char *sensitive_patterns[] = {".env",     ".env.", "credentials", "secrets",
                                           "password", ".key",  ".pem",        ".crt",
                                           ".p12",     ".pfx",  NULL};

/* Exact sensitive filenames */
static const char *sensitive_exact[] = {
    ".env",         ".env.local", ".env.production", "credentials.json",
    "secrets.json", "id_rsa",     "id_ed25519",      NULL};

/* Database extensions */
static const char *db_extensions[] = {".db", ".sqlite", ".sqlite3", NULL};

static const char *basename_of(const char *path)
{
   const char *slash = strrchr(path, '/');
   return slash ? slash + 1 : path;
}

static int matches_pattern(const char *filename, const char *pattern)
{
   return strstr(filename, pattern) != NULL;
}

static int is_sensitive_exact(const char *filename)
{
   for (int i = 0; sensitive_exact[i]; i++)
   {
      if (strcmp(filename, sensitive_exact[i]) == 0)
         return 1;
   }
   return 0;
}

static int has_db_extension(const char *filename)
{
   const char *dot = strrchr(filename, '.');
   if (!dot)
      return 0;
   for (int i = 0; db_extensions[i]; i++)
   {
      if (strcmp(dot, db_extensions[i]) == 0)
         return 1;
   }
   return 0;
}

int is_sensitive_file(const char *path)
{
   if (!path)
      return 0;
   for (int i = 0; sensitive_patterns[i]; i++)
   {
      if (strstr(path, sensitive_patterns[i]))
         return 1;
   }
   return 0;
}

classification_t classify_path(sqlite3 *db, const char *file_path)
{
   classification_t result;
   memset(&result, 0, sizeof(result));
   snprintf(result.path, sizeof(result.path), "%s", file_path);
   result.severity = SEV_GREEN;

   const char *fname = basename_of(file_path);

   /* Check exact matches first */
   if (is_sensitive_exact(fname))
   {
      result.severity = SEV_BLOCK;
      snprintf(result.reason, sizeof(result.reason), "sensitive file");
      return result;
   }

   /* Check sensitive patterns */
   for (int i = 0; sensitive_patterns[i]; i++)
   {
      if (matches_pattern(fname, sensitive_patterns[i]))
      {
         result.severity = SEV_BLOCK;
         snprintf(result.reason, sizeof(result.reason), "sensitive file pattern: %s",
                  sensitive_patterns[i]);
         return result;
      }
   }

   /* Check database extensions */
   if (has_db_extension(fname))
   {
      result.severity = SEV_RED;
      snprintf(result.reason, sizeof(result.reason), "database file");
      return result;
   }

   /* Check blast radius via index */
   if (db)
   {
      blast_radius_t br;
      memset(&br, 0, sizeof(br));
      /* Try to find what project this file belongs to */
      project_info_t projects[32];
      int pcount = index_list_projects(db, projects, 32);
      for (int p = 0; p < pcount; p++)
      {
         /* Check if file_path starts with project root (with boundary check) */
         size_t rlen = strlen(projects[p].root);
         if (strncmp(file_path, projects[p].root, rlen) == 0 &&
             (file_path[rlen] == '/' || file_path[rlen] == '\0'))
         {
            const char *rel = file_path + rlen;
            if (*rel == '/')
               rel++;
            index_blast_radius(db, projects[p].name, rel, &br);
            if (br.dependent_count >= 5)
            {
               result.severity = SEV_RED;
               snprintf(result.reason, sizeof(result.reason), "High blast radius: %d dependents",
                        br.dependent_count);
               return result;
            }
            if (br.dependent_count >= 1)
            {
               result.severity = SEV_YELLOW;
               snprintf(result.reason, sizeof(result.reason),
                        "Moderate blast radius: %d dependents", br.dependent_count);
               return result;
            }
            break;
         }
      }
   }

   return result;
}

char *normalize_path(const char *path, const char *cwd, char *buf, size_t buf_len)
{
   if (!path || !*path)
   {
      buf[0] = '\0';
      return buf;
   }

   /* ~/... paths: expand to $HOME */
   if (path[0] == '~' && (path[1] == '/' || path[1] == '\0'))
   {
      const char *home = getenv("HOME");
      if (home)
         snprintf(buf, buf_len, "%s%s", home, path + 1);
      else
         snprintf(buf, buf_len, "%s", path);
   }
   else if (path[0] == '/')
   {
      snprintf(buf, buf_len, "%s", path);
   }
   else if (cwd && *cwd)
   {
      snprintf(buf, buf_len, "%s/%s", cwd, path);
   }
   else
   {
      snprintf(buf, buf_len, "%s", path);
   }

   /* Canonicalize: resolve symlinks and .. components.
    * If the path exists, realpath resolves everything.
    * If not, try resolving the parent and appending the basename.
    * If neither resolves, keep the lexical path (safe fallback for
    * paths on nonexistent filesystems or in tests). */
   char resolved[MAX_PATH_LEN];
   if (realpath(buf, resolved))
   {
      snprintf(buf, buf_len, "%s", resolved);
      return buf;
   }

   char tmp[MAX_PATH_LEN];
   snprintf(tmp, sizeof(tmp), "%s", buf);
   char *slash = strrchr(tmp, '/');
   if (slash && slash != tmp)
   {
      *slash = '\0';
      if (realpath(tmp, resolved))
         snprintf(buf, buf_len, "%s/%s", resolved, slash + 1);
   }

   /* Lexical collapse of . and .. components for paths that could not be resolved.
    * This prevents traversal-like constructs from bypassing prefix checks. */
   if (strstr(buf, "/./") || strstr(buf, "/../"))
   {
      char parts[MAX_PATH_LEN];
      snprintf(parts, sizeof(parts), "%s", buf);
      char *stack[256];
      int depth = 0;
      char *saveptr = NULL;
      char *seg = strtok_r(parts, "/", &saveptr);
      while (seg)
      {
         if (strcmp(seg, ".") == 0)
         {
            /* skip */
         }
         else if (strcmp(seg, "..") == 0)
         {
            if (depth > 0)
               depth--;
         }
         else
         {
            stack[depth++] = seg;
         }
         seg = strtok_r(NULL, "/", &saveptr);
      }
      size_t pos = 0;
      for (int i = 0; i < depth && pos < buf_len - 2; i++)
      {
         buf[pos++] = '/';
         size_t slen = strlen(stack[i]);
         if (pos + slen >= buf_len)
            break;
         memcpy(buf + pos, stack[i], slen);
         pos += slen;
      }
      if (pos == 0 && buf_len > 1)
         buf[pos++] = '/';
      buf[pos] = '\0';
   }

   return buf;
}

/* Shared filesystem path validation for all agent tool paths.
 * Resolves symlinks via realpath, rejects traversal, rejects sensitive paths.
 * Returns NULL on success, or a static error string on failure. */
const char *guardrails_validate_file_path(const char *path, char *resolved_buf, size_t resolved_len)
{
   if (!path || !path[0])
      return "error: empty path";
   if (strstr(path, "/../") || strstr(path, "/..") == path + strlen(path) - 3)
      return "error: path traversal not allowed";
   /* Also reject leading ../ */
   if (strncmp(path, "../", 3) == 0 || strcmp(path, "..") == 0)
      return "error: path traversal not allowed";

   if (realpath(path, resolved_buf) == NULL)
   {
      /* For write, file may not exist yet -- resolve parent */
      char parent[MAX_PATH_LEN];
      snprintf(parent, sizeof(parent), "%s", path);
      char *last_slash = strrchr(parent, '/');
      if (last_slash)
      {
         *last_slash = '\0';
         if (realpath(parent, resolved_buf) == NULL)
            return NULL; /* Let fopen handle the error */
      }
      else
         return NULL;
   }

   /* Check resolved path against sensitive deny list */
   static const char *deny[] = {".ssh/",       ".gnupg/", ".aws/credentials", ".env", "/etc/shadow",
                                "/etc/passwd", NULL};
   for (int i = 0; deny[i]; i++)
   {
      if (strstr(resolved_buf, deny[i]))
         return "error: access to sensitive path denied";
   }

   /* Check for symlink escape: resolved path should not point into a
    * sensitive directory even if the original path looked benign.  Compare
    * against the same deny list (already done above on the resolved path). */

   return NULL;
}

int is_write_command(const char *command)
{
   if (!command)
      return 0;

   /* Skip leading whitespace */
   while (*command && isspace((unsigned char)*command))
      command++;

   static const char *write_cmds[] = {"rm ",    "rm\t",   "rmdir ",    "mv ",      "cp ",
                                      "chmod ", "chown ", "mkdir ",    "touch ",   "tee ",
                                      "dd ",    "ln ",    "truncate ", "install ", NULL};

   static const char *git_write_cmds[] = {
       "git commit",  "git push",      "git pull",      "git fetch", "git clone",
       "git reset",   "git checkout",  "git rebase",    "git merge", "git stash",
       "git clean",   "git branch -d", "git branch -D", "git tag",   "git add",
       "git restore", "git rm",        "git mv",        NULL};

   static const char *pkg_cmds[] = {"pip install",   "pip uninstall",   "npm install",
                                    "npm uninstall", "apt-get install", "apt install",
                                    "cargo install", "go install",      NULL};

   for (int i = 0; write_cmds[i]; i++)
   {
      if (strncmp(command, write_cmds[i], strlen(write_cmds[i])) == 0)
         return 1;
   }

   for (int i = 0; git_write_cmds[i]; i++)
   {
      if (strncmp(command, git_write_cmds[i], strlen(git_write_cmds[i])) == 0)
         return 1;
   }

   for (int i = 0; pkg_cmds[i]; i++)
   {
      if (strncmp(command, pkg_cmds[i], strlen(pkg_cmds[i])) == 0)
         return 1;
   }

   /* Check for redirection operators (with and without surrounding spaces) */
   if (strstr(command, " > ") || strstr(command, " >> "))
      return 1;
   /* Detect redirections without leading space: e.g. "echo hi>file" */
   for (const char *c = command; *c; c++)
   {
      if ((*c == '>' || (*c == '1' && c[1] == '>') || (*c == '2' && c[1] == '>')) && c != command)
         return 1;
   }

   /* Check for in-place editing flags */
   if (strstr(command, "sed -i") || strstr(command, "perl -pi"))
      return 1;

   /* Check for write commands after pipe or semicolons in compound commands */
   const char *separators[] = {" | ", " || ", " && ", "; ", NULL};
   for (int s = 0; separators[s]; s++)
   {
      const char *pos = command;
      while ((pos = strstr(pos, separators[s])) != NULL)
      {
         pos += strlen(separators[s]);
         /* Skip whitespace after separator */
         while (*pos == ' ' || *pos == '\t')
            pos++;
         /* Check if the sub-command after the separator is a write command */
         for (int i = 0; write_cmds[i]; i++)
         {
            if (strncmp(pos, write_cmds[i], strlen(write_cmds[i])) == 0)
               return 1;
         }
      }
   }

   return 0;
}

/* Check if a command is any git operation (read or write). Used to redirect
 * primary agents to aimee MCP git tools for token savings. */
int is_git_command(const char *command)
{
   if (!command)
      return 0;

   /* Skip leading whitespace */
   while (*command && isspace((unsigned char)*command))
      command++;

   /* Direct git command */
   if (strncmp(command, "git ", 4) == 0 || strncmp(command, "git\t", 4) == 0)
      return 1;

   /* gh CLI (GitHub) -- all subcommands */
   if (strncmp(command, "gh ", 3) == 0 || strncmp(command, "gh\t", 3) == 0)
      return 1;

   /* git/gh after compound operators */
   static const char *seps[] = {" | ", " || ", " && ", "; ", NULL};
   for (int s = 0; seps[s]; s++)
   {
      const char *pos = command;
      while ((pos = strstr(pos, seps[s])) != NULL)
      {
         pos += strlen(seps[s]);
         while (*pos == ' ' || *pos == '\t')
            pos++;
         if (strncmp(pos, "git ", 4) == 0 || strncmp(pos, "gh ", 3) == 0)
            return 1;
      }
   }

   return 0;
}

static int is_edit_tool(const char *tool)
{
   return strcmp(tool, "Edit") == 0 || strcmp(tool, "Write") == 0 || strcmp(tool, "MultiEdit") == 0;
}

int is_shell_tool(const char *tool)
{
   return strcmp(tool, "Bash") == 0 || strcmp(tool, "shell") == 0 ||
          strcmp(tool, "run_shell_command") == 0;
}

static int is_subagent_tool(const char *tool)
{
   return strcmp(tool, "Subagent") == 0;
}

static int looks_like_subagent_tool_name(const char *tool_name)
{
   if (!tool_name || !tool_name[0])
      return 0;

   return strcasestr(tool_name, "agent") != NULL || strcasestr(tool_name, "delegate") != NULL ||
          strcasestr(tool_name, "trigger") != NULL || strcasestr(tool_name, "spawn") != NULL ||
          strcasestr(tool_name, "remote") != NULL;
}

static int input_looks_like_subagent_request(const cJSON *root)
{
   if (!root || !cJSON_IsObject(root))
      return 0;

   static const char *keys[] = {"subagent_type", "agent_type", "prompt", "message",
                                "description",   "task",       "role",   NULL};
   int hits = 0;
   for (int i = 0; keys[i]; i++)
   {
      cJSON *item = cJSON_GetObjectItemCaseSensitive((cJSON *)root, keys[i]);
      if (item && (cJSON_IsString(item) || cJSON_IsObject(item) || cJSON_IsArray(item)))
         hits++;
   }
   return hits >= 2;
}

const char *guardrails_canonical_tool_name(const char *tool_name)
{
   if (!tool_name)
      return "";

   if (strcmp(tool_name, "bash") == 0 || strcmp(tool_name, "Bash") == 0 ||
       strcmp(tool_name, "shell") == 0 || strcmp(tool_name, "run_shell_command") == 0)
      return "Bash";
   if (strcmp(tool_name, "write_file") == 0 || strcmp(tool_name, "Write") == 0)
      return "Write";
   if (strcmp(tool_name, "read_file") == 0 || strcmp(tool_name, "Read") == 0)
      return "Read";
   if (strcmp(tool_name, "list_files") == 0 || strcmp(tool_name, "Glob") == 0)
      return "Glob";
   if (strcmp(tool_name, "grep") == 0 || strcmp(tool_name, "Grep") == 0)
      return "Grep";
   if (strcmp(tool_name, "Agent") == 0 || strcmp(tool_name, "spawn_agent") == 0 ||
       strcmp(tool_name, "RemoteTrigger") == 0)
      return "Subagent";

   return tool_name;
}

static int already_seen(session_state_t *state, const char *path)
{
   for (int i = 0; i < state->seen_count; i++)
   {
      if (strcmp(state->seen_paths[i], path) == 0)
         return 1;
   }
   return 0;
}

static void mark_seen(session_state_t *state, const char *path)
{
   if (state->seen_count >= MAX_SEEN_PATHS)
      return;
   if (already_seen(state, path))
      return;
   snprintf(state->seen_paths[state->seen_count], MAX_SEEN_LEN, "%s", path);
   state->seen_count++;
   state->dirty = 1;
}

static int check_classification(session_state_t *state, classification_t *cls, const char *mode,
                                char *msg_buf, size_t msg_len)
{
   if (cls->severity >= SEV_RED && strcmp(mode, MODE_DENY) == 0)
   {
      snprintf(msg_buf, msg_len, "BLOCKED: %s (%s)", cls->path, cls->reason);
      return 2;
   }

   if (cls->severity >= SEV_YELLOW && !already_seen(state, cls->path))
   {
      fprintf(stderr, "aimee: warning: %s (%s)\n", cls->path, cls->reason);
      mark_seen(state, cls->path);
   }

   if (cls->severity >= SEV_RED && !already_seen(state, cls->path))
   {
      fprintf(stderr, "aimee: DANGER: %s (%s)\n", cls->path, cls->reason);
      mark_seen(state, cls->path);
   }

   return 0;
}

/* Trigger a background git fetch for a workspace the first time it is touched.
 * Uses the fetched_mask bitmask in session state to avoid repeated fetches. */
static void fetch_workspace_if_needed(session_state_t *state, const config_t *cfg,
                                      const char *norm_path)
{
   if (!state || !cfg || !norm_path || state->worktree_count == 0)
      return;

   /* Find the most specific (longest) matching workspace. */
   int best = -1;
   size_t best_len = 0;
   for (int i = 0; i < cfg->workspace_count && i < 16; i++)
   {
      size_t wlen = strlen(cfg->workspaces[i]);
      if (wlen == 0)
         continue;

      /* Check if this path is inside workspace i (or its worktree) */
      int match = 0;
      if (strncmp(norm_path, cfg->workspaces[i], wlen) == 0 &&
          (norm_path[wlen] == '/' || norm_path[wlen] == '\0'))
         match = 1;

      if (!match)
      {
         /* Also check against worktree paths */
         const char *slash = strrchr(cfg->workspaces[i], '/');
         const char *ws_name = slash ? slash + 1 : cfg->workspaces[i];
         for (int j = 0; j < state->worktree_count; j++)
         {
            if (strcmp(state->worktrees[j].name, ws_name) == 0)
            {
               size_t wtlen = strlen(state->worktrees[j].path);
               if (strncmp(norm_path, state->worktrees[j].path, wtlen) == 0 &&
                   (norm_path[wtlen] == '/' || norm_path[wtlen] == '\0'))
               {
                  match = 1;
                  break;
               }
            }
         }
      }

      if (match && wlen > best_len)
      {
         best = i;
         best_len = wlen;
      }
   }

   if (best < 0)
      return;

   {
      int i = best;
      uint16_t bit = (uint16_t)(1 << i);
      if (state->fetched_mask & bit)
         return; /* already fetched this session */

      state->fetched_mask |= bit;
      state->dirty = 1;

      /* Background fetch via double-fork so the grandchild is reparented to init,
       * preventing zombie processes in the parent. */
      pid_t pid = fork();
      if (pid == 0)
      {
         /* First child: fork again and exit immediately */
         pid_t pid2 = fork();
         if (pid2 == 0)
         {
            /* Grandchild: do the actual work */
            setsid();
            const char *argv[] = {"git", "-C", cfg->workspaces[i], "fetch", "origin", NULL};
            execvp(argv[0], (char *const *)argv);
            _exit(127);
         }
         _exit(0); /* First child exits; grandchild reparented to init */
      }
      if (pid > 0)
         waitpid(pid, NULL, 0); /* Reap the short-lived first child */
      return;
   }
}

int pre_tool_check(sqlite3 *db, const char *tool_name, const char *input_json,
                   session_state_t *state, const char *guardrail_mode, const char *cwd,
                   char *msg_buf, size_t msg_len)
{
   if (!tool_name || !input_json || !state)
      return 0;

   msg_buf[0] = '\0';
   const char *mode = guardrail_mode ? guardrail_mode : MODE_APPROVE;

   /* Increment hook invocation counter for diagnostics */
   state->hook_call_count++;
   state->dirty = 1;

   cJSON *root = cJSON_Parse(input_json);
   if (!root)
      return 0;

   tool_name = guardrails_canonical_tool_name(tool_name);
   if (!is_subagent_tool(tool_name) && looks_like_subagent_tool_name(tool_name) &&
       input_looks_like_subagent_request(root))
      tool_name = "Subagent";

   /* Extract common fields */
   cJSON *fp = cJSON_GetObjectItem(root, "file_path");
   cJSON *cmd = cJSON_GetObjectItem(root, "command");

   char norm[MAX_PATH_LEN];

   /* Lazy fetch: first time a tool touches a workspace, background-fetch its remote.
    * Check file_path, path (Glob/Grep), command paths (Bash), and cwd. */
   if (state->worktree_count > 0)
   {
      config_t fcfg;
      config_load(&fcfg);
      char fnorm[MAX_PATH_LEN];

      if (fp && cJSON_IsString(fp))
      {
         normalize_path(fp->valuestring, cwd, fnorm, sizeof(fnorm));
         fetch_workspace_if_needed(state, &fcfg, fnorm);
      }
      else
      {
         /* Glob/Grep use "path" field; Bash uses cwd implicitly */
         cJSON *p = cJSON_GetObjectItem(root, "path");
         const char *check_path = (p && cJSON_IsString(p)) ? p->valuestring : cwd;
         if (check_path)
         {
            normalize_path(check_path, cwd, fnorm, sizeof(fnorm));
            fetch_workspace_if_needed(state, &fcfg, fnorm);
         }
      }
   }

   /* Plan mode check: block write tools */
   if (strcmp(state->session_mode, MODE_PLAN) == 0)
   {
      if (is_edit_tool(tool_name))
      {
         snprintf(msg_buf, msg_len, "BLOCKED: %s not allowed in plan mode", tool_name);
         cJSON_Delete(root);
         return 2;
      }
      if (is_shell_tool(tool_name) && cmd && cJSON_IsString(cmd) &&
          is_write_command(cmd->valuestring))
      {
         snprintf(msg_buf, msg_len, "BLOCKED: write command not allowed in plan mode");
         cJSON_Delete(root);
         return 2;
      }
   }

   /* Auto-provision worktrees: if no worktrees exist and a write targets a workspace,
    * create worktree entries on the fly so enforcement kicks in. This prevents writes
    * to the real repo when session-start didn't run (e.g., MCP tools, late hook setup). */
   if (state->worktree_count == 0 &&
       (is_edit_tool(tool_name) ||
        (is_shell_tool(tool_name) && cmd && cJSON_IsString(cmd) &&
         is_write_command(cmd->valuestring))))
   {
      config_t acfg;
      config_load(&acfg);

      /* Determine the target path for the write */
      const char *write_target = cwd;
      char wnorm[MAX_PATH_LEN];
      if (is_edit_tool(tool_name) && fp && cJSON_IsString(fp))
      {
         normalize_path(fp->valuestring, cwd, wnorm, sizeof(wnorm));
         write_target = wnorm;
      }

      /* Check if the write target is inside any configured workspace */
      int matched_ws = -1;
      size_t best_len = 0;
      for (int i = 0; i < acfg.workspace_count; i++)
      {
         size_t wlen = strlen(acfg.workspaces[i]);
         if (wlen == 0)
            continue;
         if (strncmp(write_target, acfg.workspaces[i], wlen) == 0 &&
             (write_target[wlen] == '/' || write_target[wlen] == '\0'))
         {
            if (wlen > best_len)
            {
               matched_ws = i;
               best_len = wlen;
            }
         }
      }

      if (matched_ws >= 0)
      {
         /* Auto-provision worktree entries for all configured workspaces */
         const char *sid = session_id();
         for (int i = 0; i < acfg.workspace_count && state->worktree_count < MAX_WORKTREES; i++)
         {
            const char *slash = strrchr(acfg.workspaces[i], '/');
            const char *ws_name = slash ? slash + 1 : acfg.workspaces[i];

            worktree_entry_t *w = &state->worktrees[state->worktree_count];
            snprintf(w->name, sizeof(w->name), "%s", ws_name);
            snprintf(w->path, sizeof(w->path), "%s/worktrees/%s/%s", config_output_dir(), sid,
                     ws_name);
            snprintf(w->workspace_root, sizeof(w->workspace_root), "%s", acfg.workspaces[i]);
            w->created = 0; /* deferred — worktree_ensure() will create on demand */
            state->worktree_count++;
         }

         /* Also add cwd if it's a git repo not already covered */
         if (cwd[0] && state->worktree_count < MAX_WORKTREES)
         {
            int covered = 0;
            for (int i = 0; i < state->worktree_count; i++)
            {
               if (strcmp(state->worktrees[i].workspace_root, cwd) == 0)
               {
                  covered = 1;
                  break;
               }
            }
            if (!covered)
            {
               /* Check for .git directory as a lightweight git repo test */
               char git_dir[MAX_PATH_LEN];
               snprintf(git_dir, sizeof(git_dir), "%s/.git", cwd);
               struct stat gst;
               if (stat(git_dir, &gst) == 0)
               {
                  const char *slash = strrchr(cwd, '/');
                  const char *ws_name = slash ? slash + 1 : cwd;

                  worktree_entry_t *w = &state->worktrees[state->worktree_count];
                  snprintf(w->name, sizeof(w->name), "%s", ws_name);
                  snprintf(w->path, sizeof(w->path), "%s/worktrees/%s/%s", config_output_dir(),
                           sid, ws_name);
                  snprintf(w->workspace_root, sizeof(w->workspace_root), "%s", cwd);
                  w->created = 0;
                  state->worktree_count++;
               }
            }
         }

         state->dirty = 1;
         fprintf(stderr,
                 "aimee: auto-provisioned %d worktree(s) — session-start did not run\n",
                 state->worktree_count);

         /* Eagerly create the worktree for the targeted workspace */
         const char *ws_slash = strrchr(acfg.workspaces[matched_ws], '/');
         const char *ws_name = ws_slash ? ws_slash + 1 : acfg.workspaces[matched_ws];
         const char *wt_path = NULL;
         for (int i = 0; i < state->worktree_count; i++)
         {
            if (strcmp(state->worktrees[i].name, ws_name) == 0)
            {
               if (worktree_ensure(&state->worktrees[i]) == 0)
                  wt_path = state->worktrees[i].path;
               break;
            }
         }

         /* Block the write regardless of whether worktree creation succeeded.
          * Writing to the real repo is never acceptable when a workspace is configured. */
         if (wt_path)
         {
            snprintf(msg_buf, msg_len,
                     "BLOCKED: write to real workspace path (worktree auto-provisioned). "
                     "Use worktree instead: %s",
                     wt_path);
         }
         else
         {
            snprintf(msg_buf, msg_len,
                     "BLOCKED: write to real workspace path. "
                     "Worktree auto-provision failed — run `aimee` to start a session first.");
         }
         cJSON_Delete(root);
         return 2;
      }
   }

   /* Worktree enforcement: block writes to real workspace paths when a worktree exists */
   if (state->worktree_count > 0)
   {
      config_t wcfg;
      config_load(&wcfg);
      /* Merge workspace roots from session worktree entries so enforcement
       * works even when config file doesn't have workspaces (CI, tests). */
      for (int wi = 0; wi < state->worktree_count && wcfg.workspace_count < 64; wi++)
      {
         if (state->worktrees[wi].workspace_root[0])
         {
            int dup = 0;
            for (int wj = 0; wj < wcfg.workspace_count; wj++)
            {
               if (strcmp(wcfg.workspaces[wj], state->worktrees[wi].workspace_root) == 0)
               {
                  dup = 1;
                  break;
               }
            }
            if (!dup)
               snprintf(wcfg.workspaces[wcfg.workspace_count++], sizeof(wcfg.workspaces[0]), "%s",
                        state->worktrees[wi].workspace_root);
         }
      }
      const char *target_path = NULL;

      if (is_edit_tool(tool_name) && fp && cJSON_IsString(fp))
      {
         normalize_path(fp->valuestring, cwd, norm, sizeof(norm));
         target_path = norm;
      }

      if (target_path)
      {
         const char *wt = worktree_for_path(state, &wcfg, target_path);
         if (wt)
         {
            fprintf(stderr, "aimee: worktree check: tool=%s path=%s -> BLOCKED (wt=%s)\n",
                    tool_name, target_path, wt);
            snprintf(msg_buf, msg_len,
                     "BLOCKED: write to real workspace path. Use worktree instead: %s", wt);
            cJSON_Delete(root);
            return 2;
         }
         fprintf(stderr, "aimee: worktree check: tool=%s path=%s -> ALLOWED\n", tool_name,
                 target_path);
      }

      /* Block bash write commands targeting real workspace paths */
      if (is_shell_tool(tool_name) && cmd && cJSON_IsString(cmd) &&
          is_write_command(cmd->valuestring))
      {
         /* Check cwd itself: git commands operate on the cwd implicitly */
         const char *cwd_wt = worktree_for_path(state, &wcfg, cwd);
         if (cwd_wt)
         {
            fprintf(stderr, "aimee: worktree check: tool=Bash cwd=%s -> BLOCKED (wt=%s)\n", cwd,
                    cwd_wt);
            snprintf(msg_buf, msg_len,
                     "BLOCKED: write command running in real workspace directory. "
                     "Use worktree instead: %s",
                     cwd_wt);
            cJSON_Delete(root);
            return 2;
         }

         /* Check cd targets: "cd /path && cmd" changes effective cwd */
         {
            const char *p = cmd->valuestring;
            while ((p = strstr(p, "cd ")) != NULL)
            {
               /* Only match "cd" at start of command or after a separator */
               if (p != cmd->valuestring)
               {
                  char prev = *(p - 1);
                  if (prev != ' ' && prev != '\t' && prev != ';' && prev != '&' && prev != '|')
                  {
                     p += 3;
                     continue;
                  }
               }
               const char *start = p + 3;
               while (*start == ' ' || *start == '\t')
                  start++;
               if (!*start)
                  break;
               /* Extract the cd target path (up to next separator or end) */
               char cd_target[MAX_PATH_LEN];
               int ti = 0;
               char quote = 0;
               for (const char *c = start; *c && ti < (int)sizeof(cd_target) - 1; c++)
               {
                  if (!quote && (*c == '\'' || *c == '"'))
                  {
                     quote = *c;
                     continue;
                  }
                  if (quote && *c == quote)
                  {
                     quote = 0;
                     continue;
                  }
                  if (!quote && (*c == ' ' || *c == '\t' || *c == ';' || *c == '&' || *c == '|'))
                     break;
                  cd_target[ti++] = *c;
               }
               cd_target[ti] = '\0';
               if (ti > 0)
               {
                  char cd_norm[MAX_PATH_LEN];
                  normalize_path(cd_target, cwd, cd_norm, sizeof(cd_norm));
                  const char *cd_wt = worktree_for_path(state, &wcfg, cd_norm);
                  if (cd_wt)
                  {
                     fprintf(stderr,
                             "aimee: worktree check: tool=Bash cd_target=%s -> BLOCKED (wt=%s)\n",
                             cd_norm, cd_wt);
                     snprintf(msg_buf, msg_len,
                              "BLOCKED: write command cd's into real workspace directory. "
                              "Use worktree instead: %s",
                              cd_wt);
                     cJSON_Delete(root);
                     return 2;
                  }
               }
               p = start;
            }
         }

         char *paths[32];
         int pcount = extract_paths_shlex(cmd->valuestring, paths, 32);
         for (int i = 0; i < pcount; i++)
         {
            normalize_path(paths[i], cwd, norm, sizeof(norm));
            const char *wt = worktree_for_path(state, &wcfg, norm);
            if (wt)
            {
               fprintf(stderr, "aimee: worktree check: tool=Bash path=%s -> BLOCKED (wt=%s)\n",
                       norm, wt);
               snprintf(msg_buf, msg_len,
                        "BLOCKED: write command targets real workspace path. "
                        "Use worktree instead: %s",
                        wt);
               for (int j = i; j < pcount; j++)
                  free(paths[j]);
               cJSON_Delete(root);
               return 2;
            }
            free(paths[i]);
         }
         fprintf(stderr, "aimee: worktree check: tool=Bash cmd='%.80s' -> ALLOWED\n",
                 cmd->valuestring);
      }

      /* Block reads (Read/Glob/Grep) to real workspace paths — but only if the
       * worktree has already been created. Before the first write, reads can go
       * to the original repo safely (non-mutating). This avoids eagerly creating
       * worktrees just because a file was read. */
      if (strcmp(tool_name, "Read") == 0 && fp && cJSON_IsString(fp))
      {
         normalize_path(fp->valuestring, cwd, norm, sizeof(norm));
         const char *wt = worktree_for_path_if_created(state, &wcfg, norm);
         if (wt)
         {
            snprintf(msg_buf, msg_len,
                     "BLOCKED: Read targets real workspace path. Use worktree instead: %s", wt);
            cJSON_Delete(root);
            return 2;
         }
      }
      if (strcmp(tool_name, "Glob") == 0 || strcmp(tool_name, "Grep") == 0)
      {
         cJSON *p = cJSON_GetObjectItem(root, "path");
         const char *check = NULL;
         if (p && cJSON_IsString(p))
         {
            normalize_path(p->valuestring, cwd, norm, sizeof(norm));
            check = norm;
         }
         else
         {
            /* No explicit path — check cwd */
            check = cwd;
         }
         if (check)
         {
            const char *wt = worktree_for_path_if_created(state, &wcfg, check);
            if (wt)
            {
               snprintf(msg_buf, msg_len,
                        "BLOCKED: %s targets real workspace path. Use worktree instead: %s",
                        tool_name, wt);
               cJSON_Delete(root);
               return 2;
            }
         }
      }
   }

   /* Git command interception: block raw git/gh commands from Bash and redirect
    * to aimee MCP git tools for token savings. Gated on config.block_raw_git. */
   if (is_shell_tool(tool_name) && cmd && cJSON_IsString(cmd) && is_git_command(cmd->valuestring))
   {
      config_t git_cfg;
      config_load(&git_cfg);
      if (git_cfg.block_raw_git)
      {
         snprintf(msg_buf, msg_len,
                  "BLOCKED: use aimee MCP git tools instead of raw git/gh commands. "
                  "Available: git_status, git_commit, git_push, git_pull, git_fetch, "
                  "git_clone, git_branch, git_log, git_diff_summary, git_stash, "
                  "git_tag, git_reset, git_restore, git_verify, git_pr "
                  "(via mcp__aimee__ prefix). "
                  "git_pr handles all gh pr/issue operations.");
         cJSON_Delete(root);
         return 2;
      }
   }

   /* Provider-native sub-agent tools must not escape the active session's
    * guardrails. Delegation should go through aimee's delegate subsystem,
    * which re-enters the same policy path with inherited session state. */
   if (is_subagent_tool(tool_name))
   {
      snprintf(msg_buf, msg_len,
               "BLOCKED: provider sub-agent tools are outside aimee's primary-agent "
               "guardrail model. Use aimee's own delegation path so the child inherits "
               "the current session guardrails.");
      fprintf(stderr, "aimee: provider sub-agent tool blocked — keep delegation inside aimee\n");
      cJSON_Delete(root);
      return 2;
   }

   /* Anti-pattern check with in-session hit tracking.
    * First match: warning (logged to stderr, included in msg_buf).
    * Third match of same pattern: block the tool call. */
   if (db)
   {
      anti_pattern_t matches[4];
      const char *file_str = (fp && cJSON_IsString(fp)) ? fp->valuestring : NULL;
      const char *cmd_str = (cmd && cJSON_IsString(cmd)) ? cmd->valuestring : NULL;
      int ap_count = anti_pattern_check(db, file_str, cmd_str, matches, 4);
      for (int i = 0; i < ap_count; i++)
      {
         /* Find or create hit counter for this pattern */
         int slot = -1;
         for (int j = 0; j < ap_hit_count; j++)
         {
            if (ap_hit_counts[j].pattern_id == matches[i].id)
            {
               slot = j;
               break;
            }
         }
         if (slot < 0 && ap_hit_count < AP_HIT_MAX_PATTERNS)
         {
            slot = ap_hit_count++;
            ap_hit_counts[slot].pattern_id = matches[i].id;
            ap_hit_counts[slot].hits = 0;
         }
         if (slot >= 0)
            ap_hit_counts[slot].hits++;

         int hits = (slot >= 0) ? ap_hit_counts[slot].hits : 1;

         if (hits >= AP_HIT_BLOCK_THRESHOLD)
         {
            snprintf(msg_buf, msg_len, "BLOCKED: anti-pattern triggered %d times this session: %s",
                     hits, matches[i].description);
            fprintf(stderr, "aimee: anti-pattern BLOCKED (%dx): %s\n", hits,
                    matches[i].description);
            cJSON_Delete(root);
            return 2;
         }
         else
         {
            /* Warning: include in msg_buf so the agent sees it */
            snprintf(msg_buf, msg_len, "WARNING: anti-pattern match (%d/%d): %s", hits,
                     AP_HIT_BLOCK_THRESHOLD, matches[i].description);
            fprintf(stderr, "aimee: anti-pattern warning (%d/%d): %s\n", hits,
                    AP_HIT_BLOCK_THRESHOLD, matches[i].description);
         }
      }
   }

   /* Drift check (warn only) */
   if (db && state->active_task_id > 0)
   {
      drift_result_t drift;
      const char *file_str = (fp && cJSON_IsString(fp)) ? fp->valuestring : NULL;
      const char *cmd_str = (cmd && cJSON_IsString(cmd)) ? cmd->valuestring : NULL;
      if (memory_check_drift(db, state->active_task_id, file_str, cmd_str, &drift) == 0 &&
          drift.drifted)
      {
         fprintf(stderr, "aimee: drift warning: %s\n", drift.message);
      }
   }

   /* Edit/Write/MultiEdit: classify file path */
   if (is_edit_tool(tool_name) && fp && cJSON_IsString(fp))
   {
      normalize_path(fp->valuestring, cwd, norm, sizeof(norm));
      classification_t cls = classify_path(db, norm);
      int rc = check_classification(state, &cls, mode, msg_buf, msg_len);
      if (rc != 0)
      {
         cJSON_Delete(root);
         return rc;
      }
   }

   /* Bash: extract and classify paths from command */
   if (is_shell_tool(tool_name) && cmd && cJSON_IsString(cmd))
   {
      char *paths[32];
      int pcount = extract_paths_shlex(cmd->valuestring, paths, 32);
      for (int i = 0; i < pcount; i++)
      {
         normalize_path(paths[i], cwd, norm, sizeof(norm));
         classification_t cls = classify_path(db, norm);
         int rc = check_classification(state, &cls, mode, msg_buf, msg_len);
         free(paths[i]);
         if (rc != 0)
         {
            for (int j = i + 1; j < pcount; j++)
               free(paths[j]);
            cJSON_Delete(root);
            return rc;
         }
      }
   }

   cJSON_Delete(root);
   return 0;
}

void post_tool_update(sqlite3 *db, const char *tool_name, const char *input_json)
{
   if (!db || !tool_name || !input_json)
      return;

   if (!is_edit_tool(tool_name))
      return;

   cJSON *root = cJSON_Parse(input_json);
   if (!root)
      return;

   cJSON *fp = cJSON_GetObjectItem(root, "file_path");
   if (fp && cJSON_IsString(fp))
   {
      /* Normalize path before comparing against project roots */
      char norm[MAX_PATH_LEN];
      normalize_path(fp->valuestring, NULL, norm, sizeof(norm));

      /* Find the project this file belongs to and re-index */
      project_info_t projects[32];
      int pcount = index_list_projects(db, projects, 32);
      for (int p = 0; p < pcount; p++)
      {
         size_t rlen = strlen(projects[p].root);
         if (strncmp(norm, projects[p].root, rlen) == 0 &&
             (norm[rlen] == '/' || norm[rlen] == '\0'))
         {
            index_scan_single_file(db, projects[p].name, projects[p].root, norm);
            break;
         }
      }
   }

   cJSON_Delete(root);
}

void session_state_load(session_state_t *state, const char *path)
{
   memset(state, 0, sizeof(*state));
   snprintf(state->session_mode, sizeof(state->session_mode), "%s", MODE_IMPLEMENT);
   snprintf(state->guardrail_mode, sizeof(state->guardrail_mode), "%s", MODE_APPROVE);

   FILE *f = fopen(path, "r");
   if (!f)
      return;

   fseek(f, 0, SEEK_END);
   long len = ftell(f);
   fseek(f, 0, SEEK_SET);

   if (len <= 0 || len > 1024 * 1024)
   {
      fclose(f);
      return;
   }

   char *buf = malloc(len + 1);
   if (!buf)
   {
      fclose(f);
      return;
   }

   size_t nread = fread(buf, 1, len, f);
   buf[nread] = '\0';
   fclose(f);

   cJSON *root = cJSON_Parse(buf);
   free(buf);
   if (!root)
      return;

   cJSON *sm = cJSON_GetObjectItem(root, "session_mode");
   if (sm && cJSON_IsString(sm))
      snprintf(state->session_mode, sizeof(state->session_mode), "%s", sm->valuestring);

   cJSON *gm = cJSON_GetObjectItem(root, "guardrail_mode");
   if (gm && cJSON_IsString(gm))
      snprintf(state->guardrail_mode, sizeof(state->guardrail_mode), "%s", gm->valuestring);

   cJSON *tid = cJSON_GetObjectItem(root, "active_task_id");
   if (tid && cJSON_IsNumber(tid))
      state->active_task_id = (int64_t)tid->valuedouble;

   cJSON *fm = cJSON_GetObjectItem(root, "fetched_mask");
   if (fm && cJSON_IsNumber(fm))
      state->fetched_mask = (uint16_t)fm->valuedouble;

   cJSON *hcc = cJSON_GetObjectItem(root, "hook_call_count");
   if (hcc && cJSON_IsNumber(hcc))
      state->hook_call_count = (int)hcc->valuedouble;

   cJSON *seen = cJSON_GetObjectItem(root, "seen_paths");
   if (seen && cJSON_IsArray(seen))
   {
      int sz = cJSON_GetArraySize(seen);
      for (int i = 0; i < sz && i < MAX_SEEN_PATHS; i++)
      {
         cJSON *item = cJSON_GetArrayItem(seen, i);
         if (item && cJSON_IsString(item))
         {
            snprintf(state->seen_paths[state->seen_count], MAX_SEEN_LEN, "%s", item->valuestring);
            state->seen_count++;
         }
      }
   }

   cJSON *wt = cJSON_GetObjectItem(root, "worktrees");
   if (wt && cJSON_IsObject(wt))
   {
      cJSON *entry = NULL;
      cJSON_ArrayForEach(entry, wt)
      {
         if (state->worktree_count >= MAX_WORKTREES)
            break;
         if (cJSON_IsString(entry) && entry->string && entry->valuestring[0])
         {
            /* Legacy format: {"name": "path"} — treat as already created */
            worktree_entry_t *w = &state->worktrees[state->worktree_count];
            snprintf(w->name, sizeof(w->name), "%s", entry->string);
            snprintf(w->path, sizeof(w->path), "%s", entry->valuestring);
            w->workspace_root[0] = '\0';
            w->created = 1;
            state->worktree_count++;
         }
         else if (cJSON_IsObject(entry) && entry->string)
         {
            /* New format: {"name": {"path": "...", "workspace_root": "...", "created": N}} */
            worktree_entry_t *w = &state->worktrees[state->worktree_count];
            snprintf(w->name, sizeof(w->name), "%s", entry->string);
            cJSON *p = cJSON_GetObjectItem(entry, "path");
            cJSON *wr = cJSON_GetObjectItem(entry, "workspace_root");
            cJSON *cr = cJSON_GetObjectItem(entry, "created");
            cJSON *bb = cJSON_GetObjectItem(entry, "base_branch");
            if (p && cJSON_IsString(p))
               snprintf(w->path, sizeof(w->path), "%s", p->valuestring);
            if (wr && cJSON_IsString(wr))
               snprintf(w->workspace_root, sizeof(w->workspace_root), "%s", wr->valuestring);
            else
               w->workspace_root[0] = '\0';
            if (bb && cJSON_IsString(bb))
               snprintf(w->base_branch, sizeof(w->base_branch), "%s", bb->valuestring);
            else
               w->base_branch[0] = '\0';
            w->created = (cr && cJSON_IsNumber(cr)) ? (int)cr->valuedouble : 1;
            state->worktree_count++;
         }
      }
   }

   cJSON *pmh = cJSON_GetObjectItem(root, "prev_main_head");
   if (pmh && cJSON_IsObject(pmh))
   {
      cJSON *item = NULL;
      cJSON_ArrayForEach(item, pmh)
      {
         if (!item->string || !cJSON_IsString(item))
            continue;
         for (int i = 0; i < state->worktree_count; i++)
         {
            if (strcmp(state->worktrees[i].name, item->string) == 0)
            {
               snprintf(state->prev_main_head[i], 64, "%s", item->valuestring);
               break;
            }
         }
      }
   }

   cJSON_Delete(root);
}

static void write_state_json(const session_state_t *state, const char *path)
{
   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, "session_mode", state->session_mode);
   cJSON_AddStringToObject(root, "guardrail_mode", state->guardrail_mode);
   cJSON_AddNumberToObject(root, "active_task_id", (double)state->active_task_id);
   cJSON_AddNumberToObject(root, "fetched_mask", (double)state->fetched_mask);
   cJSON_AddNumberToObject(root, "hook_call_count", (double)state->hook_call_count);

   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; i < state->seen_count; i++)
      cJSON_AddItemToArray(arr, cJSON_CreateString(state->seen_paths[i]));
   cJSON_AddItemToObject(root, "seen_paths", arr);

   if (state->worktree_count > 0)
   {
      cJSON *wt = cJSON_CreateObject();
      for (int i = 0; i < state->worktree_count; i++)
      {
         cJSON *entry = cJSON_CreateObject();
         cJSON_AddStringToObject(entry, "path", state->worktrees[i].path);
         if (state->worktrees[i].workspace_root[0])
            cJSON_AddStringToObject(entry, "workspace_root", state->worktrees[i].workspace_root);
         if (state->worktrees[i].base_branch[0])
            cJSON_AddStringToObject(entry, "base_branch", state->worktrees[i].base_branch);
         cJSON_AddNumberToObject(entry, "created", state->worktrees[i].created);
         cJSON_AddItemToObject(wt, state->worktrees[i].name, entry);
      }
      cJSON_AddItemToObject(root, "worktrees", wt);
   }

   /* Serialize prev_main_head map */
   {
      int has_any = 0;
      for (int i = 0; i < state->worktree_count; i++)
      {
         if (state->prev_main_head[i][0])
         {
            has_any = 1;
            break;
         }
      }
      if (has_any)
      {
         cJSON *pmh = cJSON_CreateObject();
         for (int i = 0; i < state->worktree_count; i++)
         {
            if (state->prev_main_head[i][0])
               cJSON_AddStringToObject(pmh, state->worktrees[i].name, state->prev_main_head[i]);
         }
         cJSON_AddItemToObject(root, "prev_main_head", pmh);
      }
   }

   char *json = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);

   if (json)
   {
      FILE *f = fopen(path, "w");
      if (f)
      {
         fputs(json, f);
         fclose(f);
      }
      free(json);
   }
}

void session_state_save(const session_state_t *state, const char *path)
{
   if (!state->dirty)
      return;
   write_state_json(state, path);
}

void session_state_force_save(const session_state_t *state, const char *path)
{
   write_state_json(state, path);
}

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

   /* Check if worktree directory already exists (e.g., created by another process) */
   struct stat st;
   if (stat(entry->path, &st) == 0 && S_ISDIR(st.st_mode))
   {
      entry->created = 1;
      return 0;
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
      const char *slash = strrchr(cfg->workspaces[best], '/');
      const char *ws_name = slash ? slash + 1 : cfg->workspaces[best];
      return worktree_resolve_path(state, ws_name);
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
