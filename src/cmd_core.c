/* cmd_core.c: core commands (init, setup, version, status, usage, mode, plan, implement, env,
 * contract) */
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
