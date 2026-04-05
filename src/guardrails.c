/* guardrails.c: path classification, pre-tool safety checks, session state management */
#define _GNU_SOURCE
#include "aimee.h"
#include "cJSON.h"
#include <ctype.h>
#include <dirent.h>
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

   /* Check for redirection operators (with and without surrounding spaces).
    * Must exclude fd-to-fd redirections like 2>&1 which are not file writes. */
   {
      const char *p = command;
      while ((p = strstr(p, " > ")) != NULL)
      {
         /* Skip if this is part of ">&" (fd-to-fd) */
         if (p[3] == '&')
         {
            p += 3;
            continue;
         }
         return 1;
      }
   }
   if (strstr(command, " >> "))
      return 1;
   /* Detect redirections without leading space: e.g. "echo hi>file"
    * Skip fd-to-fd redirections: N>&M (e.g. 2>&1) */
   for (const char *c = command; *c; c++)
   {
      if (c == command)
         continue;
      if (*c == '>' && c[1] == '&')
         continue; /* fd-to-fd: >&N */
      if ((*c == '1' || *c == '2') && c[1] == '>' && c[2] == '&')
         continue; /* fd-to-fd: N>&M */
      if (*c == '>' || ((*c == '1' || *c == '2') && c[1] == '>'))
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

/* Check if a Bash command contains any git write invocation
 * (commit, push, pull, reset, checkout, rebase, merge, stash, clean, tag,
 *  add, restore, rm, mv, branch -d/-D). */
static int bash_has_git_write(const char *cmd)
{
   if (!cmd)
      return 0;

   static const char *git_write_subcmds[] = {
       "commit", "push", "pull", "reset",   "checkout", "rebase", "merge",  "stash",
       "clean",  "tag",  "add",  "restore", "rm",       "mv",     "branch", NULL};

   const char *p = cmd;
   while ((p = strstr(p, "git")) != NULL)
   {
      /* Ensure "git" is at start or after a separator */
      if (p != cmd)
      {
         char prev = *(p - 1);
         if (prev != ' ' && prev != '\t' && prev != ';' && prev != '&' && prev != '|' &&
             prev != '(' && prev != '\n')
         {
            p += 3;
            continue;
         }
      }
      const char *after = p + 3;
      while (*after == ' ' || *after == '\t')
         after++;
      for (int i = 0; git_write_subcmds[i]; i++)
      {
         size_t slen = strlen(git_write_subcmds[i]);
         if (strncmp(after, git_write_subcmds[i], slen) == 0 &&
             (after[slen] == '\0' || after[slen] == ' ' || after[slen] == '\t' ||
              after[slen] == ';'))
            return 1;
      }
      p = after;
   }
   return 0;
}

/* Check if a Bash command contains a git push invocation. */
static int bash_has_git_push(const char *cmd)
{
   if (!cmd)
      return 0;
   const char *p = cmd;
   while ((p = strstr(p, "git")) != NULL)
   {
      /* Ensure "git" is at start or after a separator */
      if (p != cmd)
      {
         char prev = *(p - 1);
         if (prev != ' ' && prev != '\t' && prev != ';' && prev != '&' && prev != '|' &&
             prev != '(' && prev != '\n')
         {
            p += 3;
            continue;
         }
      }
      /* Skip "git" and whitespace, check for "push" */
      const char *after = p + 3;
      while (*after == ' ' || *after == '\t')
         after++;
      if (strncmp(after, "push", 4) == 0 &&
          (after[4] == '\0' || after[4] == ' ' || after[4] == '\t' || after[4] == ';'))
         return 1;
      p = after;
   }
   return 0;
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

   /* Worktree enforcement: block writes to real repo paths and redirect to
    * sibling worktree. Simple model: one worktree per git repo per session,
    * created as .aimee-<project>-<short-session-id> next to the project. */
   if (is_edit_tool(tool_name) || (is_shell_tool(tool_name) && cmd && cJSON_IsString(cmd) &&
                                   is_write_command(cmd->valuestring)))
   {
      /* Determine write target path */
      const char *target = cwd;
      char wnorm[MAX_PATH_LEN];
      if (is_edit_tool(tool_name) && fp && cJSON_IsString(fp))
      {
         normalize_path(fp->valuestring, cwd, wnorm, sizeof(wnorm));
         target = wnorm;
      }

      /* Find git root for the target. Skip if target is empty or not an absolute path. */
      char target_dir[MAX_PATH_LEN];
      snprintf(target_dir, sizeof(target_dir), "%s", target);
      /* If target looks like a file, use its directory */
      char *last_slash = strrchr(target_dir, '/');
      if (last_slash && last_slash != target_dir)
      {
         struct stat st;
         if (stat(target_dir, &st) != 0 || !S_ISDIR(st.st_mode))
            *last_slash = '\0';
      }

      /* For shell commands, also check if the command string itself targets a
       * worktree. This handles "cd /path/.aimee-foo-sid/src && make" where cwd
       * is the real repo but the command operates inside the worktree. */
      int cmd_targets_worktree = 0;
      if (is_shell_tool(tool_name) && cmd && cJSON_IsString(cmd))
         cmd_targets_worktree = is_aimee_worktree_path(cmd->valuestring);

      char git_root_buf[MAX_PATH_LEN];
      if (target_dir[0] == '/' &&
          git_repo_root(target_dir, git_root_buf, sizeof(git_root_buf)) == 0 &&
          !is_aimee_worktree_path(target) && !cmd_targets_worktree)
      {
         const char *sid = session_id();
         char wt_path[MAX_PATH_LEN];
         worktree_sibling_path(git_root_buf, sid, NULL, wt_path, sizeof(wt_path));

         /* If target is already inside the worktree path, allow */
         size_t wt_len = strlen(wt_path);
         if (strncmp(target, wt_path, wt_len) != 0 ||
             (target[wt_len] != '/' && target[wt_len] != '\0'))
         {
            /* Target is in the real repo, not the worktree — block and redirect */
            struct stat wt_st;
            if (stat(wt_path, &wt_st) != 0)
            {
               /* Worktree doesn't exist yet — create it */
               worktree_create_sibling(git_root_buf, sid, NULL);
            }

            /* Register mapping in session state so MCP git tools can find it */
            {
               int found = 0;
               for (int i = 0; i < state->worktree_count; i++)
               {
                  if (strcmp(state->worktrees[i].git_root, git_root_buf) == 0)
                  {
                     found = 1;
                     break;
                  }
               }
               if (!found && state->worktree_count < MAX_WORKTREES)
               {
                  worktree_mapping_t *m = &state->worktrees[state->worktree_count];
                  snprintf(m->git_root, sizeof(m->git_root), "%s", git_root_buf);
                  snprintf(m->worktree_path, sizeof(m->worktree_path), "%s", wt_path);
                  state->worktree_count++;
                  state->dirty = 1;
               }
            }

            /* For edit tools, silently rewrite the file_path to the worktree */
            if (is_edit_tool(tool_name) && fp && cJSON_IsString(fp))
            {
               size_t gr_len = strlen(git_root_buf);
               char rewritten[MAX_PATH_LEN];
               snprintf(rewritten, sizeof(rewritten), "%s%s", wt_path, target + gr_len);
               fprintf(stderr, "aimee: worktree rewrite: %s -> %s\n", target, rewritten);
               snprintf(msg_buf, msg_len, "%s", rewritten);
               cJSON_Delete(root);
               return 1; /* allow with path rewrite */
            }

            /* For bash write commands, silently rewrite by prepending cd to worktree */
            if (is_shell_tool(tool_name) && cmd && cJSON_IsString(cmd))
            {
               fprintf(stderr, "aimee: worktree rewrite (bash): cd %s && %s\n", wt_path,
                       cmd->valuestring);
               snprintf(msg_buf, msg_len, "cd %s && %s", wt_path, cmd->valuestring);
               cJSON_Delete(root);
               return 3; /* allow with command rewrite */
            }

            /* Fallback: block if we can't determine how to rewrite */
            fprintf(stderr, "aimee: worktree redirect: %s -> %s\n", target, wt_path);
            snprintf(msg_buf, msg_len, "BLOCKED: write to real repo path. Use worktree instead: %s",
                     wt_path);
            cJSON_Delete(root);
            return 2;
         }
      }
   }

   /* Main branch protection: block all git write commands via Bash on main/master */
   if (is_shell_tool(tool_name) && cmd && cJSON_IsString(cmd) &&
       bash_has_git_write(cmd->valuestring))
   {
      int brc;
      char *bbranch = run_cmd("git rev-parse --abbrev-ref HEAD 2>/dev/null", &brc);
      if (brc == 0 && bbranch)
      {
         char *bnl = strchr(bbranch, '\n');
         if (bnl)
            *bnl = '\0';
         if (strcmp(bbranch, "main") == 0 || strcmp(bbranch, "master") == 0)
         {
            free(bbranch);
            snprintf(msg_buf, msg_len,
                     "BLOCKED: git write command on main branch is not allowed. "
                     "Create a feature branch first.");
            cJSON_Delete(root);
            return 2;
         }
      }
      free(bbranch);
   }

   /* Merged-PR enforcement for Bash git push commands */
   if (is_shell_tool(tool_name) && cmd && cJSON_IsString(cmd) &&
       bash_has_git_push(cmd->valuestring))
   {
      if (check_merged_pr_for_branch())
      {
         snprintf(msg_buf, msg_len,
                  "BLOCKED: branch has a merged PR. Do not push to merged branches. "
                  "Create a new branch for new work.");
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

   /* Worktree mappings: array of {git_root, worktree_path} objects */
   cJSON *wt = cJSON_GetObjectItem(root, "worktrees");
   if (wt && cJSON_IsArray(wt))
   {
      int sz = cJSON_GetArraySize(wt);
      for (int i = 0; i < sz && state->worktree_count < MAX_WORKTREES; i++)
      {
         cJSON *entry = cJSON_GetArrayItem(wt, i);
         if (!entry || !cJSON_IsObject(entry))
            continue;
         cJSON *gr = cJSON_GetObjectItem(entry, "git_root");
         cJSON *wp = cJSON_GetObjectItem(entry, "worktree_path");
         if (gr && cJSON_IsString(gr) && wp && cJSON_IsString(wp))
         {
            worktree_mapping_t *m = &state->worktrees[state->worktree_count];
            snprintf(m->git_root, sizeof(m->git_root), "%s", gr->valuestring);
            snprintf(m->worktree_path, sizeof(m->worktree_path), "%s", wp->valuestring);
            state->worktree_count++;
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
   cJSON_AddNumberToObject(root, "hook_call_count", (double)state->hook_call_count);

   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; i < state->seen_count; i++)
      cJSON_AddItemToArray(arr, cJSON_CreateString(state->seen_paths[i]));
   cJSON_AddItemToObject(root, "seen_paths", arr);

   if (state->worktree_count > 0)
   {
      cJSON *wt = cJSON_CreateArray();
      for (int i = 0; i < state->worktree_count; i++)
      {
         cJSON *entry = cJSON_CreateObject();
         cJSON_AddStringToObject(entry, "git_root", state->worktrees[i].git_root);
         cJSON_AddStringToObject(entry, "worktree_path", state->worktrees[i].worktree_path);
         cJSON_AddItemToArray(wt, entry);
      }
      cJSON_AddItemToObject(root, "worktrees", wt);
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

/* Resolve the git repository root for a directory. If we're inside a git
 * worktree (e.g. Claude Code's .claude/worktrees/), resolve back to the
 * main repository root via --git-common-dir. */
int git_repo_root(const char *dir, char *out_root, size_t out_len)
{
   char cmd[MAX_PATH_LEN + 128];
   int rc;

   /* First try: detect if we're in a worktree by comparing --git-dir and
    * --git-common-dir. If they differ, --git-common-dir points to the main
    * repo's .git directory, and its parent is the true project root. */
   snprintf(cmd, sizeof(cmd), "git -C '%s' rev-parse --git-dir --git-common-dir 2>/dev/null", dir);
   char *out = run_cmd(cmd, &rc);
   if (rc == 0 && out && out[0])
   {
      /* Output is two lines: git-dir\ngit-common-dir\n */
      char *nl = strchr(out, '\n');
      if (nl)
      {
         *nl = '\0';
         char *git_dir = out;
         char *common_dir = nl + 1;
         /* Trim trailing newline from common_dir */
         size_t clen = strlen(common_dir);
         while (clen > 0 && (common_dir[clen - 1] == '\n' || common_dir[clen - 1] == '\r'))
            common_dir[--clen] = '\0';

         if (strcmp(git_dir, common_dir) != 0 && clen > 0)
         {
            /* We're in a worktree. common_dir is the main repo's .git dir.
             * Resolve it to an absolute path, then take its parent. */
            char abs_common[MAX_PATH_LEN];
            if (common_dir[0] == '/')
            {
               snprintf(abs_common, sizeof(abs_common), "%s", common_dir);
            }
            else
            {
               /* Relative path — resolve relative to git-dir */
               char abs_git_dir[MAX_PATH_LEN];
               if (git_dir[0] == '/')
                  snprintf(abs_git_dir, sizeof(abs_git_dir), "%s", git_dir);
               else
                  snprintf(abs_git_dir, sizeof(abs_git_dir), "%s/%s", dir, git_dir);

               snprintf(abs_common, sizeof(abs_common), "%s/%s", abs_git_dir, common_dir);
            }

            /* Canonicalize to resolve any ../ components */
            char *resolved = realpath(abs_common, NULL);
            if (resolved)
            {
               /* Strip trailing /.git to get repo root */
               size_t rlen = strlen(resolved);
               if (rlen >= 5 && strcmp(resolved + rlen - 5, "/.git") == 0)
                  resolved[rlen - 5] = '\0';
               snprintf(out_root, out_len, "%s", resolved);
               free(resolved);
               free(out);
               return 0;
            }
            free(resolved);
         }
      }
   }
   free(out);

   /* Fallback: simple --show-toplevel (works for non-worktree repos) */
   snprintf(cmd, sizeof(cmd), "git -C '%s' rev-parse --show-toplevel 2>/dev/null", dir);
   out = run_cmd(cmd, &rc);
   if (rc != 0 || !out || !out[0])
   {
      free(out);
      return -1;
   }
   size_t len = strlen(out);
   while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r'))
      out[--len] = '\0';
   snprintf(out_root, out_len, "%s", out);
   free(out);
   return 0;
}

/* (worktree_entry_init removed — replaced by simple sibling worktree model) */
