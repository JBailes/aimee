/* cmd_infra.c: infrastructure commands (git, worktree, dashboard, webchat, workspace) */
#include "aimee.h"
#include "agent_exec.h"
#include "agent_config.h"
#include "workspace.h"
#include "commands.h"
#include "dashboard.h"
#include "memory.h"
#include "cJSON.h"
#include "headers/mcp_git.h"
#include "headers/git_verify.h"
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <ctype.h>

static void print_mcp_response(cJSON *resp)
{
   if (!resp || !cJSON_IsArray(resp))
      return;
   int count = cJSON_GetArraySize(resp);
   for (int i = 0; i < count; i++)
   {
      cJSON *item = cJSON_GetArrayItem(resp, i);
      cJSON *type = cJSON_GetObjectItem(item, "type");
      cJSON *text = cJSON_GetObjectItem(item, "text");
      if (cJSON_IsString(type) && strcmp(type->valuestring, "text") == 0 && cJSON_IsString(text))
         printf("%s\n", text->valuestring);
   }
}

void cmd_git(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   if (argc < 1)
   {
      fprintf(stderr, "Usage: aimee git <status|commit|push|pull|fetch|branch|log|diff|"
                      "pr|stash|tag|reset|restore|clone|verify>\n");
      return;
   }

   const char *sub = argv[0];
   argc--;
   argv++;

   cJSON *args = cJSON_CreateObject();
   cJSON *resp = NULL;

   if (strcmp(sub, "status") == 0)
   {
      resp = handle_git_status(args);
   }
   else if (strcmp(sub, "commit") == 0)
   {
      int auto_msg = 0;
      const char *msg = NULL;
      int files_start = 0;

      for (int i = 0; i < argc; i++)
      {
         if (strcmp(argv[i], "--auto") == 0)
            auto_msg = 1;
         else if (!msg && argv[i][0] != '-')
         {
            msg = argv[i];
            files_start = i + 1;
         }
      }

      if (!msg && !auto_msg)
      {
         fprintf(stderr, "Usage: aimee git commit <message> [files...]\n");
         fprintf(stderr, "       aimee git commit --auto [files...]\n");
         cJSON_Delete(args);
         return;
      }

      if (auto_msg && !msg)
      {
         /* Generate message using delegate agent */
         fprintf(stderr, "Generating commit message...\n");
         cJSON *diff_args = cJSON_CreateObject();
         cJSON_AddBoolToObject(diff_args, "stat_only", 0);
         cJSON *diff_resp = handle_git_diff_summary(diff_args);
         cJSON_Delete(diff_args);

         char diff_text[4096] = "";
         if (diff_resp && cJSON_IsArray(diff_resp))
         {
            cJSON *item = cJSON_GetArrayItem(diff_resp, 0);
            cJSON *text = cJSON_GetObjectItem(item, "text");
            if (cJSON_IsString(text))
               snprintf(diff_text, sizeof(diff_text), "%s", text->valuestring);
         }
         cJSON_Delete(diff_resp);

         char prompt[5120];
         snprintf(prompt, sizeof(prompt),
                  "Generate a concise, one-line git commit message for these changes:\n\n%s\n\n"
                  "Output ONLY the message, no quotes or prefix.",
                  diff_text);

         sqlite3 *db = ctx_db_open(ctx);
         agent_config_t acfg;
         agent_result_t result;
         memset(&result, 0, sizeof(result));
         if (db && agent_load_config(&acfg) == 0)
         {
            /* Use cheapest agent for this simple task */
            agent_t *ag = &acfg.agents[0];
            if (agent_execute(db, ag, NULL, prompt, 128, 0.0, &result) == 0)
            {
               msg = result.response;
               fprintf(stderr, "Auto-message: %s\n", msg);
            }
         }
         ctx_db_close(ctx);
         if (!msg)
         {
            fprintf(stderr, "Error: failed to generate commit message.\n");
            cJSON_Delete(args);
            return;
         }
      }

      cJSON_AddStringToObject(args, "message", msg);
      if (files_start > 0 && files_start < argc)
      {
         cJSON *files = cJSON_CreateArray();
         for (int i = files_start; i < argc; i++)
         {
            if (argv[i][0] != '-')
               cJSON_AddItemToArray(files, cJSON_CreateString(argv[i]));
         }
         cJSON_AddItemToObject(args, "files", files);
      }
      resp = handle_git_commit(args);
      if (auto_msg)
         free((void *)msg);

      /* Trigger background re-indexing if commit succeeded */
      if (resp && cJSON_IsArray(resp))
      {
         cJSON *item = cJSON_GetArrayItem(resp, 0);
         cJSON *text = cJSON_GetObjectItem(item, "text");
         if (cJSON_IsString(text) && strncmp(text->valuestring, "committed:", 10) == 0)
         {
            char cwd[MAX_PATH_LEN];
            if (getcwd(cwd, sizeof(cwd)))
            {
               pid_t pid = fork();
               if (pid == 0)
               {
                  config_t idx_cfg;
                  config_load(&idx_cfg);
                  sqlite3 *idx_db = db_open_fast(idx_cfg.db_path);
                  if (idx_db)
                  {
                     /* Identify project name from CWD */
                     const char *proj_name = strrchr(cwd, '/');
                     proj_name = proj_name ? proj_name + 1 : cwd;
                     index_scan_project(idx_db, proj_name, cwd, 0);
                     db_stmt_cache_clear();
                     db_close(idx_db);
                  }
                  _exit(0);
               }
               if (pid > 0)
                  waitpid(pid, NULL, WNOHANG);
            }
         }
      }
   }
   else if (strcmp(sub, "push") == 0)
   {
      for (int i = 0; i < argc; i++)
      {
         if (strcmp(argv[i], "--force") == 0 || strcmp(argv[i], "-f") == 0)
            cJSON_AddBoolToObject(args, "force", 1);
      }
      resp = handle_git_push(args);
   }
   else if (strcmp(sub, "verify") == 0)
   {
      resp = handle_git_verify(args);
   }
   else if (strcmp(sub, "branch") == 0)
   {
      if (argc < 1)
      {
         cJSON_AddStringToObject(args, "action", "list");
      }
      else
      {
         const char *action = argv[0];
         if (strcmp(action, "list") == 0)
         {
            cJSON_AddStringToObject(args, "action", "list");
         }
         else if (strcmp(action, "create") == 0 || strcmp(action, "switch") == 0 ||
                  strcmp(action, "delete") == 0)
         {
            if (argc < 2)
            {
               fprintf(stderr, "Usage: aimee git branch %s <name> [base]\n", action);
               cJSON_Delete(args);
               return;
            }
            cJSON_AddStringToObject(args, "action", action);
            cJSON_AddStringToObject(args, "name", argv[1]);
            if (argc > 2 && strcmp(action, "create") == 0)
               cJSON_AddStringToObject(args, "base", argv[2]);
         }
         else
         {
            /* Assume single arg is 'switch' */
            cJSON_AddStringToObject(args, "action", "switch");
            cJSON_AddStringToObject(args, "name", action);
         }
      }
      resp = handle_git_branch(args);
   }
   else if (strcmp(sub, "log") == 0)
   {
      int count = 10;
      const char *ref = NULL;
      for (int i = 0; i < argc; i++)
      {
         if (isdigit(argv[i][0]))
            count = atoi(argv[i]);
         else if (strcmp(argv[i], "--stat") == 0)
            cJSON_AddBoolToObject(args, "diff_stat", 1);
         else
            ref = argv[i];
      }
      cJSON_AddNumberToObject(args, "count", count);
      if (ref)
         cJSON_AddStringToObject(args, "ref", ref);
      resp = handle_git_log(args);
   }
   else if (strcmp(sub, "diff") == 0)
   {
      int summary = 1;
      const char *ref = NULL;
      for (int i = 0; i < argc; i++)
      {
         if (strcmp(argv[i], "--full") == 0)
            summary = 0;
         else if (strcmp(argv[i], "--summary") == 0)
            summary = 1;
         else
            ref = argv[i];
      }
      cJSON_AddBoolToObject(args, "stat_only", summary);
      if (ref)
         cJSON_AddStringToObject(args, "ref", ref);
      resp = handle_git_diff_summary(args);
   }
   else if (strcmp(sub, "pr") == 0)
   {
      if (argc < 1)
      {
         cJSON_AddStringToObject(args, "action", "list");
      }
      else
      {
         const char *action = argv[0];
         cJSON_AddStringToObject(args, "action", action);
         if (strcmp(action, "create") == 0)
         {
            opt_parsed_t opts;
            opt_parse(argc - 1, argv + 1, NULL, &opts);
            const char *title = opt_get(&opts, "title");
            const char *body = opt_get(&opts, "body");
            const char *base = opt_get(&opts, "base");
            if (!title)
            {
               fprintf(stderr, "Usage: aimee git pr create --title \"...\" [--body \"...\"] "
                               "[--base \"...\"]\n");
               cJSON_Delete(args);
               return;
            }
            cJSON_AddStringToObject(args, "title", title);
            if (body)
               cJSON_AddStringToObject(args, "body", body);
            if (base)
               cJSON_AddStringToObject(args, "base", base);
         }
         else if (strcmp(action, "view") == 0 || strcmp(action, "merge_status") == 0)
         {
            if (argc < 2)
            {
               fprintf(stderr, "Usage: aimee git pr %s <number>\n", action);
               cJSON_Delete(args);
               return;
            }
            cJSON_AddNumberToObject(args, "number", atoi(argv[1]));
         }
      }
      resp = handle_git_pr(args);
   }
   else if (strcmp(sub, "pull") == 0)
   {
      for (int i = 0; i < argc; i++)
      {
         if (strcmp(argv[i], "--rebase") == 0 || strcmp(argv[i], "-r") == 0)
            cJSON_AddBoolToObject(args, "rebase", 1);
      }
      resp = handle_git_pull(args);
   }
   else if (strcmp(sub, "fetch") == 0)
   {
      for (int i = 0; i < argc; i++)
      {
         if (strcmp(argv[i], "--prune") == 0 || strcmp(argv[i], "-p") == 0)
            cJSON_AddBoolToObject(args, "prune", 1);
         else if (argv[i][0] != '-')
            cJSON_AddStringToObject(args, "remote", argv[i]);
      }
      resp = handle_git_fetch(args);
   }
   else if (strcmp(sub, "stash") == 0)
   {
      if (argc >= 1)
      {
         cJSON_AddStringToObject(args, "action", argv[0]);
         for (int i = 1; i < argc; i++)
         {
            if (strcmp(argv[i], "-m") == 0 && i + 1 < argc)
               cJSON_AddStringToObject(args, "message", argv[++i]);
            else if (isdigit(argv[i][0]))
               cJSON_AddNumberToObject(args, "index", atoi(argv[i]));
         }
      }
      else
      {
         cJSON_AddStringToObject(args, "action", "push");
      }
      resp = handle_git_stash(args);
   }
   else if (strcmp(sub, "tag") == 0)
   {
      if (argc < 1)
      {
         cJSON_AddStringToObject(args, "action", "list");
      }
      else if (strcmp(argv[0], "list") == 0)
      {
         cJSON_AddStringToObject(args, "action", "list");
      }
      else if (strcmp(argv[0], "delete") == 0)
      {
         cJSON_AddStringToObject(args, "action", "delete");
         if (argc > 1)
            cJSON_AddStringToObject(args, "name", argv[1]);
      }
      else
      {
         /* aimee git tag <name> [-m message] [ref] */
         cJSON_AddStringToObject(args, "action", "create");
         cJSON_AddStringToObject(args, "name", argv[0]);
         for (int i = 1; i < argc; i++)
         {
            if (strcmp(argv[i], "-m") == 0 && i + 1 < argc)
               cJSON_AddStringToObject(args, "message", argv[++i]);
            else if (argv[i][0] != '-')
               cJSON_AddStringToObject(args, "ref", argv[i]);
         }
      }
      resp = handle_git_tag(args);
   }
   else if (strcmp(sub, "reset") == 0)
   {
      for (int i = 0; i < argc; i++)
      {
         if (strcmp(argv[i], "--soft") == 0)
            cJSON_AddStringToObject(args, "mode", "soft");
         else if (strcmp(argv[i], "--mixed") == 0)
            cJSON_AddStringToObject(args, "mode", "mixed");
         else if (strcmp(argv[i], "--hard") == 0)
            cJSON_AddStringToObject(args, "mode", "hard");
         else if (argv[i][0] != '-')
            cJSON_AddStringToObject(args, "ref", argv[i]);
      }
      resp = handle_git_reset(args);
   }
   else if (strcmp(sub, "restore") == 0)
   {
      cJSON *files = cJSON_CreateArray();
      for (int i = 0; i < argc; i++)
      {
         if (strcmp(argv[i], "--staged") == 0 || strcmp(argv[i], "-S") == 0)
            cJSON_AddBoolToObject(args, "staged", 1);
         else if (strcmp(argv[i], "--source") == 0 && i + 1 < argc)
            cJSON_AddStringToObject(args, "source", argv[++i]);
         else if (argv[i][0] != '-')
            cJSON_AddItemToArray(files, cJSON_CreateString(argv[i]));
      }
      if (cJSON_GetArraySize(files) > 0)
         cJSON_AddItemToObject(args, "files", files);
      else
         cJSON_Delete(files);
      resp = handle_git_restore(args);
   }
   else if (strcmp(sub, "clone") == 0)
   {
      if (argc < 1)
      {
         fprintf(stderr, "Usage: aimee git clone <url> [path] [-b branch] [--depth N]\n");
         cJSON_Delete(args);
         return;
      }
      cJSON_AddStringToObject(args, "url", argv[0]);
      for (int i = 1; i < argc; i++)
      {
         if ((strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--branch") == 0) && i + 1 < argc)
            cJSON_AddStringToObject(args, "branch", argv[++i]);
         else if (strcmp(argv[i], "--depth") == 0 && i + 1 < argc)
            cJSON_AddNumberToObject(args, "depth", atoi(argv[++i]));
         else if (argv[i][0] != '-')
            cJSON_AddStringToObject(args, "path", argv[i]);
      }
      resp = handle_git_clone(args);
   }
   else
   {
      fprintf(stderr, "Unknown git subcommand: %s\n", sub);
   }

   if (resp)
   {
      print_mcp_response(resp);
      cJSON_Delete(resp);
   }
   cJSON_Delete(args);
}

/* Write .mcp.json in the given directory.
 * Uses "aimee mcp-serve" which proxies through aimee-server with session awareness. */
void ensure_mcp_json(const char *dir)
{
   const char *aimee_bin = "/usr/local/bin/aimee";
   struct stat st;
   if (stat(aimee_bin, &st) != 0)
      return;

   char path[MAX_PATH_LEN];
   snprintf(path, sizeof(path), "%s/.mcp.json", dir);

   /* Check if it already has the right content */
   FILE *fp = fopen(path, "r");
   if (fp)
   {
      char buf[512];
      size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
      buf[n] = '\0';
      fclose(fp);
      if (strstr(buf, "mcp-serve"))
         return; /* Already correct */
   }

   fp = fopen(path, "w");
   if (!fp)
      return;
   fprintf(fp,
           "{\n"
           "  \"mcpServers\": {\n"
           "    \"aimee\": {\n"
           "      \"command\": \"%s\",\n"
           "      \"args\": [\"mcp-serve\"]\n"
           "    }\n"
           "  }\n"
           "}\n",
           aimee_bin);
   fclose(fp);
}

/* --- cmd_init --- */

/* --- cmd_worktree: worktree management --- */

#define DEFAULT_DISK_BUDGET_BYTES (10LL * 1024 * 1024 * 1024) /* 10 GB */

static void wt_subcmd_gc(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)argc;
   (void)argv;

   config_t cfg;
   config_load(&cfg);
   /* Worktree GC no longer needed — sibling worktrees are visible to users
    * and cleaned up on session end. Use 'ls <project>-*' to find worktrees. */
   (void)db;
   (void)cfg;
   fprintf(stderr, "aimee: gc: sibling worktrees are now user-visible. "
                   "Use 'git worktree list' to find active worktrees.\n");
}

static void wt_subcmd_list(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)ctx;
   (void)argc;
   (void)argv;

   sqlite3_stmt *stmt = db_prepare(
       db, "SELECT session_id, workspace, path, created_at, last_accessed_at, size_bytes, state"
           " FROM worktrees ORDER BY created_at DESC");
   if (!stmt)
      return;

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const char *sid = (const char *)sqlite3_column_text(stmt, 0);
      const char *ws = (const char *)sqlite3_column_text(stmt, 1);
      const char *path = (const char *)sqlite3_column_text(stmt, 2);
      int64_t accessed = sqlite3_column_int64(stmt, 4);
      int64_t sz = sqlite3_column_int64(stmt, 5);
      const char *state = (const char *)sqlite3_column_text(stmt, 6);

      /* Compute hours since last access */
      time_t now = time(NULL);
      int hours_ago = (int)(difftime(now, (time_t)accessed) / 3600.0);

      printf("%-8s %-10s session=%.8s ws=%s age=%dh", state ? state : "?",
             sz > 0 ? "" : "(unknown)", sid ? sid : "?", ws ? ws : "?", hours_ago);
      if (sz > 0)
         printf(" size=%.1fMB", (double)sz / (1024.0 * 1024.0));
      printf("\n  %s\n", path ? path : "?");
      count++;
   }

   if (count == 0)
      fprintf(stderr, "No worktrees registered.\n");
   else
      fprintf(stderr, "\n%d worktree(s) total\n", count);
}

static void wt_subcmd_purge(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)ctx;
   (void)argc;
   (void)argv;

   /* Hard-delete 'deleted' rows older than 30 days */
   time_t cutoff = time(NULL) - 30 * 86400;
   sqlite3_stmt *stmt =
       db_prepare(db, "DELETE FROM worktrees WHERE state = 'deleted' AND last_accessed_at < ?");
   if (stmt)
   {
      sqlite3_bind_int64(stmt, 1, (int64_t)cutoff);
      DB_STEP_LOG(stmt, "wt_subcmd_purge");
      int changes = sqlite3_changes(db);
      fprintf(stderr, "Purged %d old audit record(s).\n", changes);
   }
}

static const subcmd_t wt_subcmds[] = {
    {"gc", "Remove stale worktrees and enforce disk budget", wt_subcmd_gc},
    {"list", "List all registered worktrees", wt_subcmd_list},
    {"purge", "Remove deleted audit records older than 30 days", wt_subcmd_purge},
    {NULL, NULL, NULL},
};

const subcmd_t *get_worktree_subcmds(void)
{
   return wt_subcmds;
}

void cmd_worktree(app_ctx_t *ctx, int argc, char **argv)
{
   sqlite3 *db = ctx_db_open(ctx);
   if (!db)
   {
      fprintf(stderr, "cmd_worktree: failed to open database\n");
      return;
   }

   const char *sub = (argc > 0) ? argv[0] : NULL;
   if (argc > 0)
   {
      argc--;
      argv++;
   }

   if (subcmd_dispatch(wt_subcmds, sub, ctx, db, argc, argv) != 0)
      subcmd_usage("worktree", wt_subcmds);

   ctx_db_close(ctx);
}

/* --- dashboard, webchat, workspace (moved from cmd_core.c) --- */

static void cmd_dashboard_cors(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;

   if (argc < 1)
   {
      fprintf(stderr, "usage: aimee dashboard cors <add|remove|list> [origin]\n");
      exit(1);
   }

   const char *action = argv[0];

   if (strcmp(action, "list") == 0)
   {
      char origins[32][CORS_ORIGIN_LEN];
      int count = dashboard_cors_list(origins, 32);
      if (count == 0)
      {
         printf("No CORS origins configured (localhost-only access).\n");
         return;
      }
      printf("Allowed CORS origins:\n");
      for (int i = 0; i < count; i++)
         printf("  %s\n", origins[i]);
   }
   else if (strcmp(action, "add") == 0)
   {
      if (argc < 2)
         fatal("usage: aimee dashboard cors add <origin>");
      if (dashboard_cors_add(argv[1]) == 0)
         printf("Added CORS origin: %s\n", argv[1]);
      else
         fatal("failed to add origin (max %d reached?)", 32);
   }
   else if (strcmp(action, "remove") == 0)
   {
      if (argc < 2)
         fatal("usage: aimee dashboard cors remove <origin>");
      if (dashboard_cors_remove(argv[1]) == 0)
         printf("Removed CORS origin: %s\n", argv[1]);
      else
         fatal("origin not found: %s", argv[1]);
   }
   else
   {
      fatal("unknown cors action: %s (use add, remove, or list)", action);
   }
}

void cmd_dashboard(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;

   /* Handle subcommand: aimee dashboard cors ... */
   if (argc >= 1 && strcmp(argv[0], "cors") == 0)
   {
      cmd_dashboard_cors(ctx, argc - 1, argv + 1);
      return;
   }

   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);
   int port = opt_get_int(&opts, "port", 0);
   dashboard_serve(port);
}

/* --- cmd_webchat --- */

#define WEBCHAT_SERVICE_NAME "aimee-webchat.service"
#define WEBCHAT_SERVICE_SRC  "systemd/aimee-webchat.service"
#define WEBCHAT_SERVICE_DEST "/etc/systemd/system/" WEBCHAT_SERVICE_NAME

static void webchat_enable(void)
{
   /* Find the service file relative to a workspace root or CWD */
   char src[MAX_PATH_LEN];
   config_t ws_cfg;
   config_load(&ws_cfg);
   if (ws_cfg.workspace_count > 0)
      snprintf(src, sizeof(src), "%s/" WEBCHAT_SERVICE_SRC, ws_cfg.workspaces[0]);
   else
      snprintf(src, sizeof(src), WEBCHAT_SERVICE_SRC);

   /* Copy service file to systemd */
   const char *cp_argv[] = {"cp", src, WEBCHAT_SERVICE_DEST, NULL};
   char *out = NULL;
   int rc = safe_exec_capture(cp_argv, &out, 1024);
   free(out);
   if (rc != 0)
   {
      fprintf(stderr, "webchat: failed to copy service file (run as root?)\n");
      return;
   }

   /* Reload systemd, enable, and start */
   const char *reload[] = {"systemctl", "daemon-reload", NULL};
   out = NULL;
   safe_exec_capture(reload, &out, 1024);
   free(out);

   const char *enable[] = {"systemctl", "enable", WEBCHAT_SERVICE_NAME, NULL};
   out = NULL;
   safe_exec_capture(enable, &out, 1024);
   free(out);

   const char *start[] = {"systemctl", "start", WEBCHAT_SERVICE_NAME, NULL};
   out = NULL;
   rc = safe_exec_capture(start, &out, 1024);
   free(out);

   if (rc == 0)
      fprintf(stderr, "webchat: enabled and started\n");
   else
      fprintf(stderr, "webchat: enabled but failed to start (check journalctl)\n");
}

static void webchat_disable(void)
{
   const char *stop[] = {"systemctl", "stop", WEBCHAT_SERVICE_NAME, NULL};
   char *out = NULL;
   safe_exec_capture(stop, &out, 1024);
   free(out);

   const char *disable[] = {"systemctl", "disable", WEBCHAT_SERVICE_NAME, NULL};
   out = NULL;
   safe_exec_capture(disable, &out, 1024);
   free(out);

   fprintf(stderr, "webchat: stopped and disabled\n");
}

static void webchat_status(void)
{
   const char *status[] = {"systemctl", "is-active", WEBCHAT_SERVICE_NAME, NULL};
   char *out = NULL;
   int rc = safe_exec_capture(status, &out, 256);

   if (rc == 0 && out)
   {
      /* Strip trailing newline */
      size_t len = strlen(out);
      while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r'))
         out[--len] = '\0';
      fprintf(stderr, "webchat: %s\n", out);
   }
   else
   {
      fprintf(stderr, "webchat: not running\n");
   }
   free(out);
}

void cmd_webchat(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;

   if (argc >= 1 && strcmp(argv[0], "enable") == 0)
   {
      webchat_enable();
      return;
   }
   if (argc >= 1 && strcmp(argv[0], "disable") == 0)
   {
      webchat_disable();
      return;
   }
   if (argc >= 1 && strcmp(argv[0], "status") == 0)
   {
      webchat_status();
      return;
   }

   /* Default: run the server directly (for manual use or systemd ExecStart) */
   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);
   int port = opt_get_int(&opts, "port", 8080);
   webchat_serve(port);
}

/* --- cmd_env --- */

static void workspace_cmd_add(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
   {
      fprintf(stderr, "Usage: aimee workspace add <path>\n");
      return;
   }

   char abs[MAX_PATH_LEN];
   if (!realpath(argv[0], abs))
   {
      fprintf(stderr, "workspace: cannot resolve path: %s\n", argv[0]);
      return;
   }

   struct stat st;
   if (stat(abs, &st) != 0 || !S_ISDIR(st.st_mode))
   {
      fprintf(stderr, "workspace: not a directory: %s\n", abs);
      return;
   }

   config_t cfg;
   config_load(&cfg);

   /* Check for duplicates */
   for (int i = 0; i < cfg.workspace_count; i++)
   {
      if (strcmp(cfg.workspaces[i], abs) == 0)
      {
         fprintf(stderr, "workspace: already registered: %s\n", abs);
         return;
      }
   }

   if (cfg.workspace_count >= 64)
   {
      fprintf(stderr, "workspace: maximum workspace count reached (64)\n");
      return;
   }

   /* Add to config */
   snprintf(cfg.workspaces[cfg.workspace_count++], MAX_PATH_LEN, "%s", abs);
   config_save(&cfg);

   /* Discover projects */
   char projects[MAX_DISCOVERED_PROJECTS][MAX_PATH_LEN];
   int count =
       workspace_discover_projects(abs, MAX_WORKSPACE_DEPTH, projects, MAX_DISCOVERED_PROJECTS);
   if (count < 0)
   {
      fprintf(stderr, "workspace: discovery failed for %s\n", abs);
      return;
   }

   fprintf(stderr, "workspace: added %s (%d project(s) discovered)\n", abs, count);

   /* Index each discovered project */
   sqlite3 *db = ctx_db_open_fast(ctx);
   if (db)
   {
      for (int i = 0; i < count; i++)
      {
         const char *name = strrchr(projects[i], '/');
         name = name ? name + 1 : projects[i];
         fprintf(stderr, "  indexing: %s\n", name);
         index_scan_project(db, name, projects[i], 0);
      }
      ctx_db_close(ctx);
   }

   if (ctx->json_output)
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddStringToObject(obj, "path", abs);
      cJSON_AddNumberToObject(obj, "projects", count);
      emit_json_ctx(obj, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      for (int i = 0; i < count; i++)
      {
         const char *name = strrchr(projects[i], '/');
         name = name ? name + 1 : projects[i];
         fprintf(stderr, "  %s\n", name);
      }
   }
}

static void workspace_cmd_list(app_ctx_t *ctx, int argc, char **argv)
{
   (void)argc;
   (void)argv;

   config_t cfg;
   config_load(&cfg);

   if (cfg.workspace_count == 0)
   {
      fprintf(stderr, "No workspaces configured. Use 'aimee workspace add <path>' to add one.\n");
      return;
   }

   sqlite3 *db = ctx_db_open_fast(ctx);
   project_info_t all_projects[256];
   int pcount = db ? index_list_projects(db, all_projects, 256) : 0;

   if (ctx->json_output)
   {
      cJSON *arr = cJSON_CreateArray();
      for (int w = 0; w < cfg.workspace_count; w++)
      {
         cJSON *ws_obj = cJSON_CreateObject();
         cJSON_AddStringToObject(ws_obj, "path", cfg.workspaces[w]);
         cJSON *projs = cJSON_AddArrayToObject(ws_obj, "projects");
         size_t ws_len = strlen(cfg.workspaces[w]);
         for (int p = 0; p < pcount; p++)
         {
            if (strncmp(all_projects[p].root, cfg.workspaces[w], ws_len) == 0 &&
                (all_projects[p].root[ws_len] == '/' || all_projects[p].root[ws_len] == '\0'))
            {
               cJSON_AddItemToArray(projs, cJSON_CreateString(all_projects[p].name));
            }
         }
         cJSON_AddItemToArray(arr, ws_obj);
      }
      emit_json_ctx(arr, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      for (int w = 0; w < cfg.workspace_count; w++)
      {
         fprintf(stderr, "%s\n", cfg.workspaces[w]);
         size_t ws_len = strlen(cfg.workspaces[w]);
         for (int p = 0; p < pcount; p++)
         {
            if (strncmp(all_projects[p].root, cfg.workspaces[w], ws_len) == 0 &&
                (all_projects[p].root[ws_len] == '/' || all_projects[p].root[ws_len] == '\0'))
            {
               fprintf(stderr, "  %s\n", all_projects[p].name);
            }
         }
      }
   }

   if (db)
      ctx_db_close(ctx);
}

static void workspace_cmd_remove(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
   {
      fprintf(stderr, "Usage: aimee workspace remove <path>\n");
      return;
   }

   config_t cfg;
   config_load(&cfg);

   /* Try to match by path (absolute or as provided) */
   char abs[MAX_PATH_LEN];
   const char *target = argv[0];
   if (realpath(argv[0], abs))
      target = abs;

   int found = -1;
   for (int i = 0; i < cfg.workspace_count; i++)
   {
      if (strcmp(cfg.workspaces[i], target) == 0)
      {
         found = i;
         break;
      }
   }

   if (found < 0)
   {
      fprintf(stderr, "workspace: not found: %s\n", argv[0]);
      return;
   }

   /* Shift remaining entries down */
   for (int i = found; i < cfg.workspace_count - 1; i++)
      snprintf(cfg.workspaces[i], MAX_PATH_LEN, "%s", cfg.workspaces[i + 1]);
   cfg.workspace_count--;
   config_save(&cfg);

   if (ctx->json_output)
      emit_ok_ctx(ctx->json_fields, ctx->response_profile);
   else
      fprintf(stderr, "workspace: removed %s\n", target);
}

void cmd_workspace(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
   {
      fprintf(stderr, "Usage: aimee workspace <add|list|remove> [options]\n");
      return;
   }

   const char *sub = argv[0];
   argc--;
   argv++;

   if (strcmp(sub, "add") == 0)
      workspace_cmd_add(ctx, argc, argv);
   else if (strcmp(sub, "list") == 0)
      workspace_cmd_list(ctx, argc, argv);
   else if (strcmp(sub, "remove") == 0)
      workspace_cmd_remove(ctx, argc, argv);
   else
   {
      fprintf(stderr, "Unknown workspace subcommand: %s\n", sub);
      fprintf(stderr, "Usage: aimee workspace <add|list|remove> [options]\n");
   }
}

/* --- aimee db --- */
