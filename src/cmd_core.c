/* cmd_core.c: simple commands (init, setup, version, mode, plan, implement, dashboard, env,
 * contract, workspace) */
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

/* --- cmd_git: CLI access to git and PR operations --- */

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
         if (strcmp(argv[i], "--skip-verify") == 0)
            cJSON_AddBoolToObject(args, "skip_verify", 1);
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

void cmd_init(app_ctx_t *ctx, int argc, char **argv)
{
   (void)argc;
   (void)argv;

   ensure_client_integrations();

   config_t cfg;
   config_load(&cfg);
   config_save(&cfg);

   ctx->db = db_open(cfg.db_path);
   if (!ctx->db)
      fatal("failed to initialize database");
   ctx_db_close(ctx);

   /* Create workspace-local .mcp.json for MCP-capable clients */
   char cwd[MAX_PATH_LEN];
   if (getcwd(cwd, sizeof(cwd)))
      ensure_mcp_json(cwd);

   if (ctx->json_output)
      emit_ok_kv_ctx("db_path", cfg.db_path, ctx->json_fields, ctx->response_profile);
   else
      fprintf(stderr, "Initialized: %s\n", cfg.db_path);
}

/* --- cmd_setup (also aliased as quickstart) --- */

void cmd_setup(app_ctx_t *ctx, int argc, char **argv)
{
   (void)argc;
   (void)argv;

   ensure_client_integrations();

   /* 1. Initialize database and config if missing */
   config_t cfg;
   config_load(&cfg);
   config_save(&cfg);
   sqlite3 *db = db_open(cfg.db_path);
   if (db)
      db_close(db);

   /* 2. If no workspaces configured, auto-add CWD */
   if (cfg.workspace_count == 0)
   {
      char cwd[MAX_PATH_LEN];
      if (getcwd(cwd, sizeof(cwd)))
      {
         snprintf(cfg.workspaces[0], MAX_PATH_LEN, "%s", cwd);
         cfg.workspace_count = 1;
         config_save(&cfg);
         fprintf(stderr, "Auto-added workspace: %s\n", cwd);
      }
   }

   /* 3. Discover and index projects in all workspaces */
   db = db_open(cfg.db_path);
   int total_projects = 0;
   for (int w = 0; w < cfg.workspace_count; w++)
   {
      char projects[MAX_DISCOVERED_PROJECTS][MAX_PATH_LEN];
      int count = workspace_discover_projects(cfg.workspaces[w], MAX_WORKSPACE_DEPTH, projects,
                                              MAX_DISCOVERED_PROJECTS);
      if (count < 0)
      {
         fprintf(stderr, "setup: cannot scan workspace %s\n", cfg.workspaces[w]);
         continue;
      }

      fprintf(stderr, "==> Workspace %s: %d project(s)\n", cfg.workspaces[w], count);

      for (int p = 0; p < count; p++)
      {
         const char *name = strrchr(projects[p], '/');
         name = name ? name + 1 : projects[p];
         if (db)
            index_scan_project(db, name, projects[p], 0);
         ensure_mcp_json(projects[p]);
         total_projects++;
      }
      ensure_mcp_json(cfg.workspaces[w]);
   }
   if (db)
   {
      db_stmt_cache_clear();
      db_close(db);
   }

   if (ctx->json_output)
      emit_ok_kv_ctx("status", "provisioned", ctx->json_fields, ctx->response_profile);
   else
      fprintf(stderr, "==> Setup complete: %d project(s) indexed.\n", total_projects);
}

/* --- cmd_db: status and pragma subcmds --- */

static void db_subcmd_status(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)ctx;
   (void)argc;
   (void)argv;

   sqlite3_stmt *stmt = NULL;
   int schema_ver = 0;
   if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &stmt, NULL) == SQLITE_OK &&
       sqlite3_step(stmt) == SQLITE_ROW)
      schema_ver = sqlite3_column_int(stmt, 0);
   if (stmt)
      sqlite3_finalize(stmt);

   char journal[32] = "unknown";
   stmt = NULL;
   if (sqlite3_prepare_v2(db, "PRAGMA journal_mode", -1, &stmt, NULL) == SQLITE_OK &&
       sqlite3_step(stmt) == SQLITE_ROW)
      snprintf(journal, sizeof(journal), "%s", sqlite3_column_text(stmt, 0));
   if (stmt)
      sqlite3_finalize(stmt);

   int sync_val = -1;
   stmt = NULL;
   if (sqlite3_prepare_v2(db, "PRAGMA synchronous", -1, &stmt, NULL) == SQLITE_OK &&
       sqlite3_step(stmt) == SQLITE_ROW)
      sync_val = sqlite3_column_int(stmt, 0);
   if (stmt)
      sqlite3_finalize(stmt);
   const char *sync_name = "unknown";
   if (sync_val == 0)
      sync_name = "OFF";
   else if (sync_val == 1)
      sync_name = "NORMAL";
   else if (sync_val == 2)
      sync_name = "FULL";
   else if (sync_val == 3)
      sync_name = "EXTRA";

   int page_size = 0, page_count = 0, freelist = 0;
   stmt = NULL;
   if (sqlite3_prepare_v2(db, "PRAGMA page_size", -1, &stmt, NULL) == SQLITE_OK &&
       sqlite3_step(stmt) == SQLITE_ROW)
      page_size = sqlite3_column_int(stmt, 0);
   if (stmt)
      sqlite3_finalize(stmt);

   stmt = NULL;
   if (sqlite3_prepare_v2(db, "PRAGMA page_count", -1, &stmt, NULL) == SQLITE_OK &&
       sqlite3_step(stmt) == SQLITE_ROW)
      page_count = sqlite3_column_int(stmt, 0);
   if (stmt)
      sqlite3_finalize(stmt);

   stmt = NULL;
   if (sqlite3_prepare_v2(db, "PRAGMA freelist_count", -1, &stmt, NULL) == SQLITE_OK &&
       sqlite3_step(stmt) == SQLITE_ROW)
      freelist = sqlite3_column_int(stmt, 0);
   if (stmt)
      sqlite3_finalize(stmt);

   int wal_pages = 0;
   stmt = NULL;
   if (sqlite3_prepare_v2(db, "PRAGMA wal_checkpoint(PASSIVE)", -1, &stmt, NULL) == SQLITE_OK &&
       sqlite3_step(stmt) == SQLITE_ROW)
      wal_pages = sqlite3_column_int(stmt, 1);
   if (stmt)
      sqlite3_finalize(stmt);

   char qc_result[64] = "error";
   stmt = NULL;
   if (sqlite3_prepare_v2(db, "PRAGMA quick_check", -1, &stmt, NULL) == SQLITE_OK &&
       sqlite3_step(stmt) == SQLITE_ROW)
   {
      const char *r = (const char *)sqlite3_column_text(stmt, 0);
      if (r)
         snprintf(qc_result, sizeof(qc_result), "%s", r);
   }
   if (stmt)
      sqlite3_finalize(stmt);

   double freelist_pct = page_count > 0 ? (100.0 * freelist / page_count) : 0.0;

   printf("Schema version: %d\n", schema_ver);
   printf("Journal mode:   %s\n", journal);
   printf("Synchronous:    %s\n", sync_name);
   printf("Page size:      %d\n", page_size);
   printf("Page count:     %d\n", page_count);
   printf("Freelist count: %d (%.2f%%)\n", freelist, freelist_pct);
   printf("WAL size:       %d pages\n", wal_pages);
   printf("Quick check:    %s\n", qc_result);
}

static void db_subcmd_pragma(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)ctx;
   (void)argc;
   (void)argv;

   static const char *pragmas[] = {
       "journal_mode", "synchronous", "busy_timeout", "foreign_keys", "wal_autocheckpoint",
       "cache_size",   "mmap_size",   "page_size",    NULL,
   };

   for (int i = 0; pragmas[i]; i++)
   {
      char sql[64];
      snprintf(sql, sizeof(sql), "PRAGMA %s", pragmas[i]);
      sqlite3_stmt *stmt = NULL;
      if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK &&
          sqlite3_step(stmt) == SQLITE_ROW)
      {
         printf("%-22s %s\n", pragmas[i], sqlite3_column_text(stmt, 0));
      }
      if (stmt)
         sqlite3_finalize(stmt);
   }
}

/* --- cmd_version --- */

void cmd_version(app_ctx_t *ctx, int argc, char **argv)
{
   (void)argc;
   (void)argv;

   if (ctx->json_output)
      emit_ok_kv_ctx("version", AIMEE_VERSION, ctx->json_fields, ctx->response_profile);
   else
      printf("aimee %s\n", AIMEE_VERSION);
}

/* --- cmd_status --- */

void cmd_status(app_ctx_t *ctx, int argc, char **argv)
{
   (void)argc;
   (void)argv;

   printf("aimee %s\n", AIMEE_VERSION);

   /* Database status */
   config_t cfg;
   config_load(&cfg);
   sqlite3 *db = db_open(cfg.db_path);
   if (db)
   {
      int schema_ver = 0;
      sqlite3_stmt *stmt = NULL;
      if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &stmt, NULL) == SQLITE_OK &&
          sqlite3_step(stmt) == SQLITE_ROW)
         schema_ver = sqlite3_column_int(stmt, 0);
      if (stmt)
         sqlite3_finalize(stmt);
      printf("Database:  ok (schema v%d)\n", schema_ver);
      db_close(db);
   }
   else
   {
      printf("Database:  error (cannot open)\n");
   }

   /* Provider status: test each configured agent */
   agent_config_t acfg;
   if (agent_load_config(&acfg) == 0)
   {
      agent_http_init();
      for (int i = 0; i < acfg.agent_count; i++)
      {
         agent_t *ag = &acfg.agents[i];
         if (!ag->enabled)
         {
            printf("Provider:  %s (disabled)\n", ag->name);
            continue;
         }

         /* Quick connectivity test */
         sqlite3 *tdb = db_open(cfg.db_path);
         if (!tdb)
         {
            printf("Provider:  %s (cannot open db for test)\n", ag->name);
            continue;
         }
         agent_result_t result;
         memset(&result, 0, sizeof(result));
         int rc = agent_execute(tdb, ag, NULL, "Respond with 'ok'.", 64, 0.0, &result);
         db_close(tdb);

         if (rc == 0)
            printf("Provider:  %s (ok, %dms latency)\n", ag->name, result.latency_ms);
         else
         {
            const provider_health_t *h = provider_health_get(ag->provider);
            if (h && h->error[0])
               printf("Provider:  %s (%s)\n", ag->name, h->error);
            else
               printf("Provider:  %s (error: %s)\n", ag->name, result.error);
         }
         free(result.response);
      }
      agent_http_cleanup();
   }

   /* Worktree count */
   db = db_open(cfg.db_path);
   if (db)
   {
      sqlite3_stmt *stmt = NULL;
      int active = 0;
      if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM worktrees WHERE state = 'active'", -1, &stmt,
                             NULL) == SQLITE_OK &&
          sqlite3_step(stmt) == SQLITE_ROW)
         active = sqlite3_column_int(stmt, 0);
      if (stmt)
         sqlite3_finalize(stmt);
      printf("Worktrees: %d active\n", active);
      db_close(db);
   }

   if (ctx->json_output)
      emit_ok_ctx(ctx->json_fields, ctx->response_profile);
}

/* --- cmd_usage --- */

void cmd_usage(app_ctx_t *ctx, int argc, char **argv)
{
   (void)argc;
   (void)argv;

   config_t cfg;
   config_load(&cfg);
   sqlite3 *db = db_open(cfg.db_path);
   if (!db)
   {
      fprintf(stderr, "aimee: cannot open database\n");
      return;
   }

   /* Parse --last flag for time window (default: all time) */
   const char *window = NULL;
   for (int i = 0; i < argc; i++)
   {
      if (strncmp(argv[i], "--last=", 7) == 0)
         window = argv[i] + 7;
      else if (strcmp(argv[i], "--last") == 0 && i + 1 < argc)
         window = argv[++i];
   }

   char where_clause[256] = "";
   if (window)
   {
      int hours = 24;
      if (strstr(window, "h"))
         hours = atoi(window);
      else if (strstr(window, "d"))
         hours = atoi(window) * 24;
      else if (strstr(window, "w"))
         hours = atoi(window) * 24 * 7;
      else
         hours = atoi(window);
      if (hours <= 0)
         hours = 24;
      snprintf(where_clause, sizeof(where_clause),
               " WHERE created_at >= datetime('now', '-%d hours')", hours);
   }

   /* Overall totals from agent_log */
   int total_prompt = 0, total_completion = 0, total_calls = 0;
   {
      char sql[512];
      snprintf(sql, sizeof(sql),
               "SELECT COUNT(*), COALESCE(SUM(prompt_tokens), 0),"
               " COALESCE(SUM(completion_tokens), 0)"
               " FROM agent_log%s",
               where_clause);
      sqlite3_stmt *stmt = NULL;
      if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK &&
          sqlite3_step(stmt) == SQLITE_ROW)
      {
         total_calls = sqlite3_column_int(stmt, 0);
         total_prompt = sqlite3_column_int(stmt, 1);
         total_completion = sqlite3_column_int(stmt, 2);
      }
      if (stmt)
         sqlite3_finalize(stmt);
   }

   int total_tokens = total_prompt + total_completion;

   /* Breakdown by role */
   if (ctx->json_output)
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddNumberToObject(obj, "total_calls", total_calls);
      cJSON_AddNumberToObject(obj, "prompt_tokens", total_prompt);
      cJSON_AddNumberToObject(obj, "completion_tokens", total_completion);
      cJSON_AddNumberToObject(obj, "total_tokens", total_tokens);

      /* By role */
      cJSON *roles = cJSON_AddArrayToObject(obj, "by_role");
      {
         char sql[512];
         snprintf(sql, sizeof(sql),
                  "SELECT role, COUNT(*), SUM(prompt_tokens), SUM(completion_tokens)"
                  " FROM agent_log%s GROUP BY role ORDER BY SUM(prompt_tokens) + "
                  "SUM(completion_tokens) DESC",
                  where_clause);
         sqlite3_stmt *stmt = NULL;
         if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK)
         {
            while (sqlite3_step(stmt) == SQLITE_ROW)
            {
               cJSON *r = cJSON_CreateObject();
               const char *role = (const char *)sqlite3_column_text(stmt, 0);
               cJSON_AddStringToObject(r, "role", role ? role : "");
               cJSON_AddNumberToObject(r, "calls", sqlite3_column_int(stmt, 1));
               cJSON_AddNumberToObject(r, "prompt_tokens", sqlite3_column_int(stmt, 2));
               cJSON_AddNumberToObject(r, "completion_tokens", sqlite3_column_int(stmt, 3));
               cJSON_AddItemToArray(roles, r);
            }
         }
         if (stmt)
            sqlite3_finalize(stmt);
      }

      /* By agent */
      cJSON *agents = cJSON_AddArrayToObject(obj, "by_agent");
      {
         char sql[512];
         snprintf(sql, sizeof(sql),
                  "SELECT agent_name, COUNT(*), SUM(prompt_tokens), SUM(completion_tokens)"
                  " FROM agent_log%s GROUP BY agent_name ORDER BY SUM(prompt_tokens) + "
                  "SUM(completion_tokens) DESC",
                  where_clause);
         sqlite3_stmt *stmt = NULL;
         if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK)
         {
            while (sqlite3_step(stmt) == SQLITE_ROW)
            {
               cJSON *a = cJSON_CreateObject();
               const char *name = (const char *)sqlite3_column_text(stmt, 0);
               cJSON_AddStringToObject(a, "agent", name ? name : "");
               cJSON_AddNumberToObject(a, "calls", sqlite3_column_int(stmt, 1));
               cJSON_AddNumberToObject(a, "prompt_tokens", sqlite3_column_int(stmt, 2));
               cJSON_AddNumberToObject(a, "completion_tokens", sqlite3_column_int(stmt, 3));
               cJSON_AddItemToArray(agents, a);
            }
         }
         if (stmt)
            sqlite3_finalize(stmt);
      }

      char *json = cJSON_PrintUnformatted(obj);
      printf("%s\n", json);
      free(json);
      cJSON_Delete(obj);
   }
   else
   {
      printf("Token Usage%s:\n", window ? "" : " (all time)");
      printf("  Total calls:       %d\n", total_calls);
      printf("  Prompt tokens:     %d\n", total_prompt);
      printf("  Completion tokens: %d\n", total_completion);
      printf("  Total tokens:      %d\n\n", total_tokens);

      /* By role */
      printf("By role:\n");
      {
         char sql[512];
         snprintf(sql, sizeof(sql),
                  "SELECT role, COUNT(*), SUM(prompt_tokens) + SUM(completion_tokens)"
                  " FROM agent_log%s GROUP BY role ORDER BY 3 DESC LIMIT 10",
                  where_clause);
         sqlite3_stmt *stmt = NULL;
         if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK)
         {
            while (sqlite3_step(stmt) == SQLITE_ROW)
            {
               const char *role = (const char *)sqlite3_column_text(stmt, 0);
               int calls = sqlite3_column_int(stmt, 1);
               int tokens = sqlite3_column_int(stmt, 2);
               printf("  %-16s %5d calls  %8d tokens\n", role && role[0] ? role : "(none)", calls,
                      tokens);
            }
         }
         if (stmt)
            sqlite3_finalize(stmt);
      }

      /* By agent */
      printf("\nBy agent:\n");
      {
         char sql[512];
         snprintf(sql, sizeof(sql),
                  "SELECT agent_name, COUNT(*), SUM(prompt_tokens) + SUM(completion_tokens)"
                  " FROM agent_log%s GROUP BY agent_name ORDER BY 3 DESC LIMIT 10",
                  where_clause);
         sqlite3_stmt *stmt = NULL;
         if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK)
         {
            while (sqlite3_step(stmt) == SQLITE_ROW)
            {
               const char *name = (const char *)sqlite3_column_text(stmt, 0);
               int calls = sqlite3_column_int(stmt, 1);
               int tokens = sqlite3_column_int(stmt, 2);
               printf("  %-16s %5d calls  %8d tokens\n", name && name[0] ? name : "(none)", calls,
                      tokens);
            }
         }
         if (stmt)
            sqlite3_finalize(stmt);
      }
   }

   db_close(db);
}

/* --- cmd_mode --- */

void cmd_mode(app_ctx_t *ctx, int argc, char **argv)
{
   char state_path[MAX_PATH_LEN];
   session_state_path(state_path, sizeof(state_path));

   session_state_t state;
   session_state_load(&state, state_path);

   if (argc < 1)
   {
      /* Get mode */
      if (ctx->json_output)
         emit_ok_kv_ctx("mode", state.session_mode[0] ? state.session_mode : MODE_PLAN,
                        ctx->json_fields, ctx->response_profile);
      else
         printf("%s\n", state.session_mode[0] ? state.session_mode : MODE_PLAN);
      return;
   }

   const char *mode = argv[0];
   if (strcmp(mode, MODE_PLAN) != 0 && strcmp(mode, MODE_IMPLEMENT) != 0)
      fatal("invalid mode: %s (must be 'plan' or 'implement')", mode);

   snprintf(state.session_mode, sizeof(state.session_mode), "%s", mode);
   state.dirty = 1;
   session_state_force_save(&state, state_path);

   if (ctx->json_output)
      emit_ok_kv_ctx("mode", mode, ctx->json_fields, ctx->response_profile);
   else
      fprintf(stderr, "Mode set to: %s\n", mode);
}

/* --- cmd_plan / cmd_implement --- */

void cmd_plan(app_ctx_t *ctx, int argc, char **argv)
{
   (void)argc;
   (void)argv;
   char *args[] = {(char *)MODE_PLAN};
   cmd_mode(ctx, 1, args);
}

void cmd_implement(app_ctx_t *ctx, int argc, char **argv)
{
   (void)argc;
   (void)argv;
   char *args[] = {(char *)MODE_IMPLEMENT};
   cmd_mode(ctx, 1, args);
}

/* --- cmd_dashboard --- */

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

void cmd_env(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;

   if (argc < 1 || strcmp(argv[0], "detect") == 0)
   {
      sqlite3 *db = ctx_db_open(ctx);
      if (!db)
         fatal("cannot open database");
      agent_introspect_env(db);
      /* Display results */
      static const char *sql = "SELECT key, value FROM env_capabilities ORDER BY key";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (stmt)
      {
         sqlite3_reset(stmt);
         while (sqlite3_step(stmt) == SQLITE_ROW)
         {
            printf("%-24s %s\n", (const char *)sqlite3_column_text(stmt, 0),
                   (const char *)sqlite3_column_text(stmt, 1));
         }
      }
      ctx_db_close(ctx);
   }
}

/* --- cmd_contract --- */

void cmd_contract(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   (void)argc;
   (void)argv;

   /* Show the project contract for the current directory */
   char *contract = agent_load_project_contract(NULL);
   if (contract)
   {
      printf("%s\n", contract);
      free(contract);
   }
   else
   {
      printf("No .aimee/project.yaml found in current directory.\n");
   }
}

/* --- cmd_workspace --- */

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

static void db_subcmd_backup(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)ctx;
   (void)db;
   config_t cfg;
   config_load(&cfg);
   const char *out = (argc > 0) ? argv[0] : NULL;
   if (db_backup(cfg.db_path, out) != 0)
   {
      fprintf(stderr, "aimee db backup: failed\n");
      exit(1);
   }
}

static void db_subcmd_check(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)ctx;
   (void)db;
   (void)argc;
   (void)argv;
   config_t cfg;
   config_load(&cfg);
   if (db_check(cfg.db_path, 1) != 0)
      exit(1);
}

static void db_subcmd_recover(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)ctx;
   (void)db;
   int force = 0;
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--force") == 0)
         force = 1;
   }
   config_t cfg;
   config_load(&cfg);
   if (db_recover(cfg.db_path, force) != 0)
      exit(1);
}

static void db_subcmd_next_migration(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)ctx;
   (void)db;
   (void)argc;
   (void)argv;
   printf("%d\n", db_next_migration_version());
}

static void db_subcmd_validate_migrations(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)ctx;
   (void)db;
   (void)argc;
   (void)argv;
   char err[256];
   if (db_validate_migrations(err, sizeof(err)) != 0)
   {
      fprintf(stderr, "FAIL: %s\n", err);
      exit(1);
   }
   printf("OK: all migration IDs are valid and ordered.\n");
}

static const subcmd_t db_subcmds[] = {
    {"status", "Show database health and statistics", db_subcmd_status},
    {"pragma", "Dump current pragma values", db_subcmd_pragma},
    {"backup", "Create a manual database backup", db_subcmd_backup},
    {"check", "Run full integrity check", db_subcmd_check},
    {"recover", "Recover from most recent valid backup", db_subcmd_recover},
    {"next-migration", "Print next available migration version", db_subcmd_next_migration},
    {"validate-migrations", "Check migration IDs for ordering issues",
     db_subcmd_validate_migrations},
    {NULL, NULL, NULL},
};

const subcmd_t *get_db_subcmds(void)
{
   return db_subcmds;
}

void cmd_db(app_ctx_t *ctx, int argc, char **argv)
{
   sqlite3 *db = ctx_db_open_fast(ctx);
   if (!db)
   {
      fprintf(stderr, "cmd_db: failed to open database\n");
      return;
   }

   const char *sub = (argc > 0) ? argv[0] : NULL;
   if (argc > 0)
   {
      argc--;
      argv++;
   }

   if (subcmd_dispatch(db_subcmds, sub, ctx, db, argc, argv) != 0)
      subcmd_usage("db", db_subcmds);

   ctx_db_close(ctx);
}

/* --- cmd_worktree: worktree management --- */

#define DEFAULT_DISK_BUDGET_BYTES (10LL * 1024 * 1024 * 1024) /* 10 GB */

static void wt_subcmd_gc(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)argc;
   (void)argv;

   config_t cfg;
   config_load(&cfg);
   int cleaned = worktree_gc(db, &cfg, DEFAULT_DISK_BUDGET_BYTES, 1);
   if (cleaned == 0)
      fprintf(stderr, "aimee: gc: no stale worktrees found\n");
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

/* --- Export/Import --- */

/* Write a table to a JSONL file. Returns number of rows exported. */
static int export_table_jsonl(sqlite3 *db, const char *sql, const char *path)
{
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return -1;

   FILE *f = fopen(path, "w");
   if (!f)
      return -1;

   int count = 0;
   sqlite3_reset(stmt);
   while (sqlite3_step(stmt) == SQLITE_ROW)
   {
      int ncols = sqlite3_column_count(stmt);
      cJSON *obj = cJSON_CreateObject();
      for (int i = 0; i < ncols; i++)
      {
         const char *name = sqlite3_column_name(stmt, i);
         int type = sqlite3_column_type(stmt, i);
         switch (type)
         {
         case SQLITE_INTEGER:
            cJSON_AddNumberToObject(obj, name, sqlite3_column_int64(stmt, i));
            break;
         case SQLITE_FLOAT:
            cJSON_AddNumberToObject(obj, name, sqlite3_column_double(stmt, i));
            break;
         case SQLITE_TEXT:
            cJSON_AddStringToObject(obj, name, (const char *)sqlite3_column_text(stmt, i));
            break;
         case SQLITE_NULL:
            cJSON_AddNullToObject(obj, name);
            break;
         default:
            break;
         }
      }
      char *line = cJSON_PrintUnformatted(obj);
      if (line)
      {
         fprintf(f, "%s\n", line);
         free(line);
         count++;
      }
      cJSON_Delete(obj);
   }
   fclose(f);
   return count;
}

void cmd_export(app_ctx_t *ctx, int argc, char **argv)
{
   const char *category = "all";
   const char *output = "aimee-export";

   for (int i = 0; i < argc; i++)
   {
      if (strncmp(argv[i], "--category=", 11) == 0 || strncmp(argv[i], "--category", 10) == 0)
      {
         if (argv[i][10] == '=')
            category = argv[i] + 11;
         else if (i + 1 < argc)
            category = argv[++i];
      }
      else if (strncmp(argv[i], "--output=", 9) == 0 || strncmp(argv[i], "--output", 8) == 0)
      {
         if (argv[i][8] == '=')
            output = argv[i] + 9;
         else if (i + 1 < argc)
            output = argv[++i];
      }
      else if (strcmp(argv[i], "--help") == 0)
      {
         fprintf(stderr, "Usage: aimee export [--category <cat>] [--output <path>]\n"
                         "Categories: all, memories, rules, config, decisions\n");
         return;
      }
   }

   sqlite3 *db = ctx_db_open(ctx);
   if (!db)
      return;

   /* Create output directory */
   mkdir(output, 0755);

   int total = 0;

   /* Manifest */
   {
      char path[4096];
      snprintf(path, sizeof(path), "%s/manifest.json", output);
      cJSON *m = cJSON_CreateObject();
      cJSON_AddStringToObject(m, "version", AIMEE_VERSION);
      cJSON_AddStringToObject(m, "category", category);

      char ts[32];
      now_utc(ts, sizeof(ts));
      cJSON_AddStringToObject(m, "exported_at", ts);
      cJSON_AddStringToObject(m, "platform", "linux");

      char *json = cJSON_Print(m);
      cJSON_Delete(m);
      if (json)
      {
         FILE *f = fopen(path, "w");
         if (f)
         {
            fprintf(f, "%s\n", json);
            fclose(f);
         }
         free(json);
      }
   }

   /* Memories */
   if (strcmp(category, "all") == 0 || strcmp(category, "memories") == 0)
   {
      char path[4096];
      snprintf(path, sizeof(path), "%s/memories.jsonl", output);
      int n = export_table_jsonl(db,
                                 "SELECT tier, kind, key, content, confidence, "
                                 "use_count, source_session, created_at, updated_at "
                                 "FROM memories ORDER BY id",
                                 path);
      if (n >= 0)
      {
         printf("Exported %d memories\n", n);
         total += n;
      }
   }

   /* Rules */
   if (strcmp(category, "all") == 0 || strcmp(category, "rules") == 0)
   {
      char path[4096];
      snprintf(path, sizeof(path), "%s/rules.jsonl", output);
      int n = export_table_jsonl(db,
                                 "SELECT polarity, title, description, weight, "
                                 "domain, created_at, updated_at "
                                 "FROM rules ORDER BY id",
                                 path);
      if (n >= 0)
      {
         printf("Exported %d rules\n", n);
         total += n;
      }
   }

   /* Decisions */
   if (strcmp(category, "all") == 0 || strcmp(category, "decisions") == 0)
   {
      char path[4096];
      snprintf(path, sizeof(path), "%s/decisions.jsonl", output);
      int n = export_table_jsonl(db,
                                 "SELECT tier, kind, key, content, confidence, "
                                 "source_session, created_at "
                                 "FROM memories WHERE kind = 'decision' ORDER BY id",
                                 path);
      if (n >= 0)
      {
         printf("Exported %d decisions\n", n);
         total += n;
      }
   }

   /* Config */
   if (strcmp(category, "all") == 0 || strcmp(category, "config") == 0)
   {
      config_t cfg;
      if (config_load(&cfg) == 0)
      {
         char path[4096];
         snprintf(path, sizeof(path), "%s/config.json", output);
         cJSON *c = cJSON_CreateObject();
         cJSON_AddStringToObject(c, "guardrail_mode", config_guardrail_mode(&cfg));
         /* Redact sensitive fields */
         cJSON_AddStringToObject(c, "note", "Sensitive fields redacted. Re-configure on target.");

         char *json = cJSON_Print(c);
         cJSON_Delete(c);
         if (json)
         {
            FILE *f = fopen(path, "w");
            if (f)
            {
               fprintf(f, "%s\n", json);
               fclose(f);
            }
            free(json);
         }
         printf("Exported config\n");
      }
   }

   printf("Export complete: %d items to %s/\n", total, output);
   ctx_db_close(ctx);
}

void cmd_import(app_ctx_t *ctx, int argc, char **argv)
{
   const char *input = NULL;
   const char *category = "all";
   const char *conflict = "skip";

   for (int i = 0; i < argc; i++)
   {
      if (strncmp(argv[i], "--category=", 11) == 0)
         category = argv[i] + 11;
      else if (strncmp(argv[i], "--conflict=", 11) == 0)
         conflict = argv[i] + 11;
      else if (strcmp(argv[i], "--help") == 0)
      {
         fprintf(stderr,
                 "Usage: aimee import <path> [--category <cat>] [--conflict skip|overwrite]\n");
         return;
      }
      else if (argv[i][0] != '-')
      {
         input = argv[i];
      }
   }

   if (!input)
   {
      fprintf(stderr,
              "Usage: aimee import <path> [--category <cat>] [--conflict skip|overwrite]\n");
      return;
   }

   /* Read manifest */
   char manifest_path[4096];
   snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.json", input);
   FILE *mf = fopen(manifest_path, "r");
   if (!mf)
   {
      fprintf(stderr, "error: cannot open %s (is this an aimee export directory?)\n",
              manifest_path);
      return;
   }
   char mbuf[8192];
   size_t mlen = fread(mbuf, 1, sizeof(mbuf) - 1, mf);
   fclose(mf);
   mbuf[mlen] = '\0';
   cJSON *manifest = cJSON_Parse(mbuf);
   if (!manifest)
   {
      fprintf(stderr, "error: invalid manifest.json\n");
      return;
   }
   cJSON *jver = cJSON_GetObjectItemCaseSensitive(manifest, "version");
   printf("Importing from %s (exported by aimee %s)\n", input,
          cJSON_IsString(jver) ? jver->valuestring : "unknown");
   cJSON_Delete(manifest);

   sqlite3 *db = ctx_db_open(ctx);
   if (!db)
      return;

   int total_imported = 0;
   int total_skipped = 0;

   /* Import memories */
   if (strcmp(category, "all") == 0 || strcmp(category, "memories") == 0)
   {
      char path[4096];
      snprintf(path, sizeof(path), "%s/memories.jsonl", input);
      FILE *f = fopen(path, "r");
      if (f)
      {
         char line[65536];
         int imported = 0, skipped = 0;
         while (fgets(line, sizeof(line), f))
         {
            cJSON *obj = cJSON_Parse(line);
            if (!obj)
               continue;

            cJSON *jkey = cJSON_GetObjectItemCaseSensitive(obj, "key");
            cJSON *jcontent = cJSON_GetObjectItemCaseSensitive(obj, "content");
            cJSON *jtier = cJSON_GetObjectItemCaseSensitive(obj, "tier");
            cJSON *jkind = cJSON_GetObjectItemCaseSensitive(obj, "kind");
            cJSON *jconf = cJSON_GetObjectItemCaseSensitive(obj, "confidence");

            if (!cJSON_IsString(jkey) || !cJSON_IsString(jcontent))
            {
               cJSON_Delete(obj);
               continue;
            }

            /* Check for duplicate by key */
            if (strcmp(conflict, "skip") == 0)
            {
               static const char *dup_sql = "SELECT 1 FROM memories WHERE key = ? LIMIT 1";
               sqlite3_stmt *ds = db_prepare(db, dup_sql);
               if (ds)
               {
                  sqlite3_bind_text(ds, 1, jkey->valuestring, -1, SQLITE_TRANSIENT);
                  int exists = (sqlite3_step(ds) == SQLITE_ROW);
                  sqlite3_reset(ds);
                  if (exists)
                  {
                     skipped++;
                     cJSON_Delete(obj);
                     continue;
                  }
               }
            }

            memory_t m;
            const char *tier = cJSON_IsString(jtier) ? jtier->valuestring : "L2";
            const char *kind = cJSON_IsString(jkind) ? jkind->valuestring : "fact";
            double conf = cJSON_IsNumber(jconf) ? jconf->valuedouble : 1.0;
            memory_insert(db, tier, kind, jkey->valuestring, jcontent->valuestring, conf, "import",
                          &m);
            imported++;
            cJSON_Delete(obj);
         }
         fclose(f);
         printf("Memories: %d imported, %d skipped\n", imported, skipped);
         total_imported += imported;
         total_skipped += skipped;
      }
   }

   /* Import rules */
   if (strcmp(category, "all") == 0 || strcmp(category, "rules") == 0)
   {
      char path[4096];
      snprintf(path, sizeof(path), "%s/rules.jsonl", input);
      FILE *f = fopen(path, "r");
      if (f)
      {
         char rline[65536];
         int imported = 0, skipped = 0;
         while (fgets(rline, sizeof(rline), f))
         {
            cJSON *obj = cJSON_Parse(rline);
            if (!obj)
               continue;

            cJSON *jpol = cJSON_GetObjectItemCaseSensitive(obj, "polarity");
            cJSON *jtitle = cJSON_GetObjectItemCaseSensitive(obj, "title");
            cJSON *jdesc = cJSON_GetObjectItemCaseSensitive(obj, "description");
            cJSON *jwt = cJSON_GetObjectItemCaseSensitive(obj, "weight");

            if (!cJSON_IsString(jtitle))
            {
               cJSON_Delete(obj);
               continue;
            }

            static const char *ins_sql =
                "INSERT INTO rules (polarity, title, description, weight, created_at, updated_at)"
                " VALUES (?, ?, ?, ?, datetime('now'), datetime('now'))";
            sqlite3_stmt *ins = db_prepare(db, ins_sql);
            if (ins)
            {
               sqlite3_bind_text(ins, 1, cJSON_IsString(jpol) ? jpol->valuestring : "positive", -1,
                                 SQLITE_TRANSIENT);
               sqlite3_bind_text(ins, 2, jtitle->valuestring, -1, SQLITE_TRANSIENT);
               sqlite3_bind_text(ins, 3, cJSON_IsString(jdesc) ? jdesc->valuestring : "", -1,
                                 SQLITE_TRANSIENT);
               sqlite3_bind_int(ins, 4, cJSON_IsNumber(jwt) ? (int)jwt->valuedouble : 5);
               DB_STEP_LOG(ins, "cmd_core");
               sqlite3_reset(ins);
               imported++;
            }
            cJSON_Delete(obj);
         }
         fclose(f);
         printf("Rules: %d imported, %d skipped\n", imported, skipped);
         total_imported += imported;
         total_skipped += skipped;
      }
   }

   printf("Import complete: %d imported, %d skipped\n", total_imported, total_skipped);
   ctx_db_close(ctx);
}

/* --- cmd_config --- */

typedef struct
{
   const char *key;
   size_t offset;
   size_t size;
   int is_bool;
} config_field_t;

static const config_field_t config_fields[] = {
    {"provider", offsetof(config_t, provider), sizeof(((config_t *)0)->provider), 0},
    {"use_builtin_cli", offsetof(config_t, use_builtin_cli), sizeof(int), 1},
    {"openai_endpoint", offsetof(config_t, openai_endpoint),
     sizeof(((config_t *)0)->openai_endpoint), 0},
    {"openai_model", offsetof(config_t, openai_model), sizeof(((config_t *)0)->openai_model), 0},
    {"openai_key_cmd", offsetof(config_t, openai_key_cmd), sizeof(((config_t *)0)->openai_key_cmd),
     0},
    {"guardrail_mode", offsetof(config_t, guardrail_mode), sizeof(((config_t *)0)->guardrail_mode),
     0},
    {"embedding_command", offsetof(config_t, embedding_command),
     sizeof(((config_t *)0)->embedding_command), 0},
    {"block_raw_git", offsetof(config_t, block_raw_git), sizeof(int), 1},
    {"cross_verify", offsetof(config_t, cross_verify), sizeof(int), 1},
    {NULL, 0, 0, 0},
};

static const config_field_t *config_field_lookup(const char *key)
{
   for (int i = 0; config_fields[i].key; i++)
   {
      if (strcmp(key, config_fields[i].key) == 0)
         return &config_fields[i];
   }
   return NULL;
}

void cmd_config(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
   {
      fprintf(stderr, "Usage: aimee config <show|get|set>\n");
      fprintf(stderr, "  show              Show all config as JSON\n");
      fprintf(stderr, "  get <key>         Get a config value\n");
      fprintf(stderr, "  set <key> <value> Set a config value\n");
      fprintf(stderr, "\nShortcuts:\n");
      fprintf(stderr, "  aimee use <provider>\n");
      fprintf(stderr, "  aimee provider [name]\n");
      fprintf(stderr, "\nKeys: ");
      for (int i = 0; config_fields[i].key; i++)
         fprintf(stderr, "%s%s", i ? ", " : "", config_fields[i].key);
      fprintf(stderr, "\n");
      return;
   }

   const char *sub = argv[0];

   if (strcmp(sub, "show") == 0)
   {
      config_t cfg;
      if (config_load(&cfg) < 0)
      {
         fprintf(stderr, "Failed to load config\n");
         return;
      }
      config_save(&cfg); /* ensure file exists */

      /* Read and print the file directly for faithful JSON output */
      FILE *fp = fopen(config_default_path(), "r");
      if (!fp)
      {
         fprintf(stderr, "Cannot read %s\n", config_default_path());
         return;
      }
      char buf[4096];
      size_t n;
      while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
         fwrite(buf, 1, n, stdout);
      fclose(fp);
      (void)ctx;
      return;
   }

   if (strcmp(sub, "get") == 0)
   {
      if (argc < 2)
      {
         fprintf(stderr, "Usage: aimee config get <key>\n");
         return;
      }
      const char *key = argv[1];
      const config_field_t *f = config_field_lookup(key);
      if (!f)
      {
         fprintf(stderr, "Unknown config key: %s\n", key);
         return;
      }

      config_t cfg;
      if (config_load(&cfg) < 0)
      {
         fprintf(stderr, "Failed to load config\n");
         return;
      }

      if (f->is_bool)
      {
         int val = *(int *)((char *)&cfg + f->offset);
         printf("%s\n", val ? "true" : "false");
      }
      else
      {
         const char *val = (const char *)&cfg + f->offset;
         printf("%s\n", val[0] ? val : "(unset)");
      }
      (void)ctx;
      return;
   }

   if (strcmp(sub, "set") == 0)
   {
      if (argc < 3)
      {
         fprintf(stderr, "Usage: aimee config set <key> <value>\n");
         return;
      }
      const char *key = argv[1];
      const char *value = argv[2];
      const config_field_t *f = config_field_lookup(key);
      if (!f)
      {
         fprintf(stderr, "Unknown config key: %s\n", key);
         return;
      }

      config_t cfg;
      if (config_load(&cfg) < 0)
      {
         fprintf(stderr, "Failed to load config\n");
         return;
      }

      if (f->is_bool)
      {
         int *ptr = (int *)((char *)&cfg + f->offset);
         if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0)
            *ptr = 1;
         else if (strcmp(value, "false") == 0 || strcmp(value, "0") == 0)
            *ptr = 0;
         else
         {
            fprintf(stderr, "Invalid boolean value: %s (use true/false)\n", value);
            return;
         }
      }
      else
      {
         char *ptr = (char *)&cfg + f->offset;
         snprintf(ptr, f->size, "%s", value);
      }

      if (config_save(&cfg) < 0)
      {
         fprintf(stderr, "Failed to save config\n");
         return;
      }
      fprintf(stderr, "%s = %s\n", key, value);

      /* Provider is now config-only, no manifest to update */
      (void)ctx;
      return;
   }

   fprintf(stderr, "Unknown subcommand: %s\nUsage: aimee config <show|get|set>\n", sub);
}
