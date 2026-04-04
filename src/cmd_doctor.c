/* cmd_doctor.c: diagnostic command that checks all critical subsystems */
#define _XOPEN_SOURCE 700
#include "aimee.h"
#include "agent_config.h"
#include "commands.h"
#include "workspace.h"
#include "memory.h"
#include "cJSON.h"
#include "headers/secret_store.h"
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>

/* Check result status */
typedef enum
{
   CHECK_OK = 0,
   CHECK_WARN,
   CHECK_ERROR
} check_status_t;

typedef struct
{
   const char *name;
   check_status_t status;
   char message[256];
   char remediation[256];
} check_result_t;

#define MAX_CHECKS 9

/* --- Individual check functions --- */

static void check_database(check_result_t *r, config_t *cfg)
{
   r->name = "Database";

   struct stat st;
   if (stat(cfg->db_path, &st) != 0)
   {
      r->status = CHECK_ERROR;
      snprintf(r->message, sizeof(r->message), "not found: %s", cfg->db_path);
      snprintf(r->remediation, sizeof(r->remediation), "Run 'aimee init' to create the database");
      return;
   }

   if (access(cfg->db_path, R_OK | W_OK) != 0)
   {
      r->status = CHECK_ERROR;
      snprintf(r->message, sizeof(r->message), "not writable: %s", cfg->db_path);
      snprintf(r->remediation, sizeof(r->remediation), "Check file permissions on %s",
               cfg->db_path);
      return;
   }

   sqlite3 *db = db_open(cfg->db_path);
   if (!db)
   {
      r->status = CHECK_ERROR;
      snprintf(r->message, sizeof(r->message), "cannot open");
      snprintf(r->remediation, sizeof(r->remediation), "Run 'aimee init' to recreate the database");
      return;
   }

   int schema_ver = 0;
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &stmt, NULL) == SQLITE_OK &&
       sqlite3_step(stmt) == SQLITE_ROW)
      schema_ver = sqlite3_column_int(stmt, 0);
   if (stmt)
      sqlite3_finalize(stmt);

   /* Check FTS5 integrity */
   int fts_ok = 1;
   stmt = NULL;
   if (sqlite3_prepare_v2(db,
                          "INSERT INTO memories_fts(memories_fts, rank, content) "
                          "VALUES('integrity-check', 1, 1)",
                          -1, &stmt, NULL) == SQLITE_OK)
   {
      int rc = sqlite3_step(stmt);
      if (rc != SQLITE_OK && rc != SQLITE_DONE)
         fts_ok = 0;
      sqlite3_finalize(stmt);
   }

   /* Get file size */
   long size_kb = st.st_size / 1024;

   db_close(db);

   if (!fts_ok)
   {
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message), "schema v%d, %ldKB, FTS5 index corrupted",
               schema_ver, size_kb);
      snprintf(r->remediation, sizeof(r->remediation),
               "Run 'aimee doctor --fix' to rebuild FTS5 index");
   }
   else
   {
      r->status = CHECK_OK;
      if (size_kb >= 1024)
         snprintf(r->message, sizeof(r->message), "schema v%d, %.1fMB", schema_ver,
                  (double)size_kb / 1024.0);
      else
         snprintf(r->message, sizeof(r->message), "schema v%d, %ldKB", schema_ver, size_kb);
   }
}

static void check_server(check_result_t *r)
{
   r->name = "Server";

   /* Check if aimee-server process is running */
   FILE *fp = popen("pgrep -x aimee-server 2>/dev/null", "r");
   if (!fp)
   {
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message), "cannot check (pgrep unavailable)");
      return;
   }

   char buf[64];
   int found = 0;
   pid_t pid = 0;
   if (fgets(buf, sizeof(buf), fp))
   {
      found = 1;
      pid = (pid_t)atoi(buf);
   }
   pclose(fp);

   if (!found)
   {
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message), "not running");
      snprintf(r->remediation, sizeof(r->remediation),
               "Server starts automatically on first aimee command");
      return;
   }

   /* Check uptime from /proc if available */
   char proc_path[128];
   snprintf(proc_path, sizeof(proc_path), "/proc/%d/stat", (int)pid);
   struct stat st;
   if (stat(proc_path, &st) == 0)
   {
      time_t uptime_secs = time(NULL) - st.st_mtime;
      if (uptime_secs < 60)
         snprintf(r->message, sizeof(r->message), "pid %d, uptime %lds", (int)pid,
                  (long)uptime_secs);
      else if (uptime_secs < 3600)
         snprintf(r->message, sizeof(r->message), "pid %d, uptime %ldm", (int)pid,
                  (long)(uptime_secs / 60));
      else
         snprintf(r->message, sizeof(r->message), "pid %d, uptime %ldh%ldm", (int)pid,
                  (long)(uptime_secs / 3600), (long)((uptime_secs % 3600) / 60));
   }
   else
   {
      snprintf(r->message, sizeof(r->message), "pid %d", (int)pid);
   }
   r->status = CHECK_OK;
}

static void check_config(check_result_t *r, config_t *cfg)
{
   r->name = "Config";

   const char *path = config_default_path();
   struct stat st;
   if (stat(path, &st) != 0)
   {
      r->status = CHECK_ERROR;
      snprintf(r->message, sizeof(r->message), "not found");
      snprintf(r->remediation, sizeof(r->remediation), "Run 'aimee init' to create config");
      return;
   }

   if (cfg->workspace_count == 0)
   {
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message), "no workspaces configured");
      snprintf(r->remediation, sizeof(r->remediation),
               "Run 'aimee workspace add <path>' or 'aimee setup'");
      return;
   }

   /* Verify workspace paths exist */
   int missing = 0;
   for (int i = 0; i < cfg->workspace_count; i++)
   {
      if (stat(cfg->workspaces[i], &st) != 0 || !S_ISDIR(st.st_mode))
         missing++;
   }

   if (missing > 0)
   {
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message), "%d workspace(s), %d missing", cfg->workspace_count,
               missing);
      snprintf(r->remediation, sizeof(r->remediation),
               "Run 'aimee workspace list' and remove stale entries");
   }
   else
   {
      r->status = CHECK_OK;
      snprintf(r->message, sizeof(r->message), "%d workspace(s)", cfg->workspace_count);
   }
}

static void check_agents(check_result_t *r)
{
   r->name = "Agents";

   agent_config_t acfg;
   if (agent_load_config(&acfg) != 0)
   {
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message), "agents.json not found or invalid");
      snprintf(r->remediation, sizeof(r->remediation),
               "Run 'aimee agent list' to check agent configuration");
      return;
   }

   int enabled = 0;
   for (int i = 0; i < acfg.agent_count; i++)
   {
      if (acfg.agents[i].enabled)
         enabled++;
   }

   if (enabled == 0)
   {
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message), "%d agents configured, none enabled",
               acfg.agent_count);
      snprintf(r->remediation, sizeof(r->remediation),
               "Enable an agent in agents.json or run 'aimee config provider <name>'");
   }
   else
   {
      r->status = CHECK_OK;
      snprintf(r->message, sizeof(r->message), "%d configured, %d enabled", acfg.agent_count,
               enabled);
   }
}

static void check_hooks(check_result_t *r)
{
   r->name = "Hooks";

   const char *home = getenv("HOME");
   if (!home)
   {
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message), "HOME not set");
      return;
   }

   int found = 0;
   char path[MAX_PATH_LEN];
   struct stat st;

   /* Claude Code settings */
   snprintf(path, sizeof(path), "%s/.claude/settings.json", home);
   if (stat(path, &st) == 0)
      found++;

   /* Codex config */
   snprintf(path, sizeof(path), "%s/.codex/config.json", home);
   if (stat(path, &st) == 0)
      found++;

   if (found == 0)
   {
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message), "no AI tool hooks detected");
      snprintf(r->remediation, sizeof(r->remediation),
               "Run 'aimee init' in a project directory to register hooks");
   }
   else
   {
      r->status = CHECK_OK;
      snprintf(r->message, sizeof(r->message), "%d tool integration(s) found", found);
   }
}

static void check_mcp(check_result_t *r, config_t *cfg)
{
   r->name = "MCP";

   int found = 0;
   struct stat st;
   char path[MAX_PATH_LEN];

   for (int i = 0; i < cfg->workspace_count; i++)
   {
      snprintf(path, sizeof(path), "%s/.mcp.json", cfg->workspaces[i]);
      if (stat(path, &st) == 0)
         found++;
   }

   if (cfg->workspace_count == 0)
   {
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message), "no workspaces to check");
      snprintf(r->remediation, sizeof(r->remediation), "Run 'aimee setup' to configure workspaces");
   }
   else if (found == 0)
   {
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message), "no .mcp.json in any workspace");
      snprintf(r->remediation, sizeof(r->remediation),
               "Run 'aimee init' in workspace directories to create .mcp.json");
   }
   else
   {
      r->status = CHECK_OK;
      snprintf(r->message, sizeof(r->message), "%d/%d workspace(s)", found, cfg->workspace_count);
   }
}

static void check_secrets(check_result_t *r)
{
   r->name = "Secrets";

   const secret_backend_t *backend = secret_backend();
   if (!backend)
   {
      r->status = CHECK_ERROR;
      snprintf(r->message, sizeof(r->message), "no secret backend available");
      snprintf(r->remediation, sizeof(r->remediation),
               "Install libsecret or check file-based secret store");
      return;
   }

   /* Try loading common keys to see if any are stored */
   char buf[256];
   int count = 0;
   static const char *keys[] = {"anthropic_api_key", "openai_api_key", "gemini_api_key",
                                "openrouter_api_key"};
   for (int i = 0; i < 4; i++)
   {
      if (secret_load(keys[i], buf, sizeof(buf)) == 0 && buf[0])
         count++;
      memset(buf, 0, sizeof(buf));
   }

   r->status = CHECK_OK;
   snprintf(r->message, sizeof(r->message), "%s backend, %d key(s) stored", backend->name, count);
}

static void check_index(check_result_t *r, config_t *cfg)
{
   r->name = "Index";

   sqlite3 *db = db_open(cfg->db_path);
   if (!db)
   {
      r->status = CHECK_ERROR;
      snprintf(r->message, sizeof(r->message), "cannot open database");
      return;
   }

   /* Count indexed projects */
   sqlite3_stmt *stmt = NULL;
   int project_count = 0;
   if (sqlite3_prepare_v2(db, "SELECT COUNT(DISTINCT project) FROM symbols", -1, &stmt, NULL) ==
           SQLITE_OK &&
       sqlite3_step(stmt) == SQLITE_ROW)
      project_count = sqlite3_column_int(stmt, 0);
   if (stmt)
      sqlite3_finalize(stmt);

   if (project_count == 0)
   {
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message), "no projects indexed");
      snprintf(r->remediation, sizeof(r->remediation), "Run 'aimee setup' to index projects");
      db_close(db);
      return;
   }

   /* Check staleness: most recent indexed timestamp */
   stmt = NULL;
   char ts_buf[64] = {0};
   if (sqlite3_prepare_v2(db, "SELECT MAX(indexed_at) FROM symbols", -1, &stmt, NULL) ==
           SQLITE_OK &&
       sqlite3_step(stmt) == SQLITE_ROW)
   {
      const char *last = (const char *)sqlite3_column_text(stmt, 0);
      if (last)
         snprintf(ts_buf, sizeof(ts_buf), "%s", last);
   }
   if (stmt)
      sqlite3_finalize(stmt);

   db_close(db);

   /* Parse timestamp and check if older than 24h */
   int stale = 0;
   if (ts_buf[0])
   {
      struct tm tm_parsed;
      memset(&tm_parsed, 0, sizeof(tm_parsed));
      if (strptime(ts_buf, "%Y-%m-%dT%H:%M:%S", &tm_parsed))
      {
         time_t indexed_time = mktime(&tm_parsed);
         time_t now = time(NULL);
         double hours = difftime(now, indexed_time) / 3600.0;
         if (hours > 24.0)
         {
            stale = 1;
            r->status = CHECK_WARN;
            snprintf(r->message, sizeof(r->message),
                     "%d project(s), stale (%.0fh since last index)", project_count, hours);
            snprintf(r->remediation, sizeof(r->remediation), "Run 'aimee setup' to re-index");
         }
      }
   }

   if (!stale)
   {
      r->status = CHECK_OK;
      snprintf(r->message, sizeof(r->message), "%d project(s)", project_count);
   }
}

static void check_memory(check_result_t *r, config_t *cfg)
{
   r->name = "Memory";

   sqlite3 *db = db_open(cfg->db_path);
   if (!db)
   {
      r->status = CHECK_ERROR;
      snprintf(r->message, sizeof(r->message), "cannot open database");
      return;
   }

   int l2_count = 0, l3_count = 0, total = 0;
   sqlite3_stmt *stmt = NULL;

   if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM memories", -1, &stmt, NULL) == SQLITE_OK &&
       sqlite3_step(stmt) == SQLITE_ROW)
      total = sqlite3_column_int(stmt, 0);
   if (stmt)
      sqlite3_finalize(stmt);

   stmt = NULL;
   if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM memories WHERE tier = 'L2'", -1, &stmt, NULL) ==
           SQLITE_OK &&
       sqlite3_step(stmt) == SQLITE_ROW)
      l2_count = sqlite3_column_int(stmt, 0);
   if (stmt)
      sqlite3_finalize(stmt);

   stmt = NULL;
   if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM memories WHERE tier = 'L3'", -1, &stmt, NULL) ==
           SQLITE_OK &&
       sqlite3_step(stmt) == SQLITE_ROW)
      l3_count = sqlite3_column_int(stmt, 0);
   if (stmt)
      sqlite3_finalize(stmt);

   /* Check for orphaned L0 entries older than 7 days */
   int orphaned_l0 = 0;
   stmt = NULL;
   if (sqlite3_prepare_v2(db,
                          "SELECT COUNT(*) FROM memories WHERE tier = 'L0' "
                          "AND created_at < datetime('now', '-7 days')",
                          -1, &stmt, NULL) == SQLITE_OK &&
       sqlite3_step(stmt) == SQLITE_ROW)
      orphaned_l0 = sqlite3_column_int(stmt, 0);
   if (stmt)
      sqlite3_finalize(stmt);

   db_close(db);

   if (total == 0)
   {
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message), "empty");
      snprintf(r->remediation, sizeof(r->remediation),
               "Memory populates as you use aimee in sessions");
   }
   else if (orphaned_l0 > 0)
   {
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message), "L2: %d, L3: %d, %d orphaned L0 entries", l2_count,
               l3_count, orphaned_l0);
      snprintf(r->remediation, sizeof(r->remediation),
               "Run 'aimee doctor --fix' to prune orphaned L0 memories");
   }
   else
   {
      r->status = CHECK_OK;
      snprintf(r->message, sizeof(r->message), "L2: %d facts, L3: %d episodes", l2_count, l3_count);
   }
}

/* --- Fix functions --- */

static int fix_orphaned_l0(config_t *cfg)
{
   sqlite3 *db = db_open(cfg->db_path);
   if (!db)
      return -1;

   char *errmsg = NULL;
   int rc = sqlite3_exec(db,
                         "DELETE FROM memories WHERE tier = 'L0' "
                         "AND created_at < datetime('now', '-7 days')",
                         NULL, NULL, &errmsg);
   int changes = sqlite3_changes(db);
   if (errmsg)
   {
      fprintf(stderr, "  fix: L0 prune failed: %s\n", errmsg);
      sqlite3_free(errmsg);
   }
   else if (changes > 0)
   {
      fprintf(stderr, "  fix: pruned %d orphaned L0 memories\n", changes);
   }
   db_close(db);
   return rc == SQLITE_OK ? 0 : -1;
}

static int fix_reindex(config_t *cfg)
{
   sqlite3 *db = db_open(cfg->db_path);
   if (!db)
      return -1;

   int total = 0;
   for (int w = 0; w < cfg->workspace_count; w++)
   {
      char projects[32][MAX_PATH_LEN];
      int count = workspace_discover_projects(cfg->workspaces[w], 3, projects, 32);
      for (int p = 0; p < count; p++)
      {
         const char *name = strrchr(projects[p], '/');
         name = name ? name + 1 : projects[p];
         index_scan_project(db, name, projects[p], 0);
         total++;
      }
   }
   db_close(db);

   if (total > 0)
      fprintf(stderr, "  fix: re-indexed %d project(s)\n", total);
   return 0;
}

static int fix_hooks(void)
{
   ensure_client_integrations();
   fprintf(stderr, "  fix: re-registered client integrations\n");
   return 0;
}

/* --- Status formatting helpers --- */

static const char *status_label(check_status_t s)
{
   switch (s)
   {
   case CHECK_OK:
      return "OK";
   case CHECK_WARN:
      return "WARN";
   case CHECK_ERROR:
      return "ERROR";
   }
   return "?";
}

/* --- Main command handler --- */

void cmd_doctor(app_ctx_t *ctx, int argc, char **argv)
{
   int do_fix = 0;
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--fix") == 0)
         do_fix = 1;
   }

   config_t cfg;
   config_load(&cfg);

   check_result_t checks[MAX_CHECKS];
   memset(checks, 0, sizeof(checks));

   int n = 0;
   check_database(&checks[n++], &cfg);
   check_server(&checks[n++]);
   check_config(&checks[n++], &cfg);
   check_agents(&checks[n++]);
   check_hooks(&checks[n++]);
   check_mcp(&checks[n++], &cfg);
   check_secrets(&checks[n++]);
   check_index(&checks[n++], &cfg);
   check_memory(&checks[n++], &cfg);

   int warnings = 0, errors = 0;
   for (int i = 0; i < n; i++)
   {
      if (checks[i].status == CHECK_WARN)
         warnings++;
      else if (checks[i].status == CHECK_ERROR)
         errors++;
   }

   if (ctx->json_output)
   {
      cJSON *root = cJSON_CreateObject();
      cJSON_AddStringToObject(root, "version", AIMEE_VERSION);
      cJSON *arr = cJSON_AddArrayToObject(root, "checks");

      for (int i = 0; i < n; i++)
      {
         cJSON *item = cJSON_CreateObject();
         cJSON_AddStringToObject(item, "name", checks[i].name);
         cJSON_AddStringToObject(item, "status", status_label(checks[i].status));
         cJSON_AddStringToObject(item, "message", checks[i].message);
         if (checks[i].remediation[0])
            cJSON_AddStringToObject(item, "remediation", checks[i].remediation);
         cJSON_AddItemToArray(arr, item);
      }

      cJSON_AddNumberToObject(root, "warnings", warnings);
      cJSON_AddNumberToObject(root, "errors", errors);

      emit_json_ctx(root, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      fprintf(stderr, "aimee doctor v%s\n\n", AIMEE_VERSION);

      for (int i = 0; i < n; i++)
      {
         int name_len = (int)strlen(checks[i].name);
         int dots = 20 - name_len;
         if (dots < 2)
            dots = 2;

         fprintf(stderr, "  %s ", checks[i].name);
         for (int d = 0; d < dots; d++)
            fputc('.', stderr);
         fprintf(stderr, " %-5s", status_label(checks[i].status));
         if (checks[i].message[0])
            fprintf(stderr, " (%s)", checks[i].message);
         fprintf(stderr, "\n");
      }

      fprintf(stderr, "\n%d warning(s), %d error(s)\n", warnings, errors);

      if ((warnings > 0 || errors > 0) && !do_fix)
         fprintf(stderr, "Run with --fix to auto-repair detected issues.\n");
   }

   if (do_fix)
   {
      fprintf(stderr, "\nApplying fixes...\n");
      fix_orphaned_l0(&cfg);
      fix_reindex(&cfg);
      fix_hooks();
      fprintf(stderr, "Done.\n");
   }

   /* Exit code: 0 = all pass, 1 = warnings, 2 = errors */
   if (errors > 0)
      exit(2);
   else if (warnings > 0)
      exit(1);
}
