/* cmd_data.c: data management commands (db, export, import, config) */
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

   if (subcmd_dispatch(get_db_subcmds(), sub, ctx, db, argc, argv) != 0)
      subcmd_usage("db", get_db_subcmds());

   ctx_db_close(ctx);
}

/* --- cmd_db: status and pragma subcmds --- */

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

typedef enum
{
   CFG_STRING,
   CFG_BOOL,
   CFG_INT
} config_field_type_t;

typedef struct
{
   const char *key;
   size_t offset;
   size_t size;
   int is_bool; /* legacy: 1 for bool fields */
   config_field_type_t type;
} config_field_t;

static const config_field_t config_fields[] = {
    {"provider", offsetof(config_t, provider), sizeof(((config_t *)0)->provider), 0, CFG_STRING},
    {"use_builtin_cli", offsetof(config_t, use_builtin_cli), sizeof(int), 1, CFG_BOOL},
    {"openai_endpoint", offsetof(config_t, openai_endpoint),
     sizeof(((config_t *)0)->openai_endpoint), 0, CFG_STRING},
    {"openai_model", offsetof(config_t, openai_model), sizeof(((config_t *)0)->openai_model), 0,
     CFG_STRING},
    {"openai_key_cmd", offsetof(config_t, openai_key_cmd), sizeof(((config_t *)0)->openai_key_cmd),
     0, CFG_STRING},
    {"guardrail_mode", offsetof(config_t, guardrail_mode), sizeof(((config_t *)0)->guardrail_mode),
     0, CFG_STRING},
    {"embedding_command", offsetof(config_t, embedding_command),
     sizeof(((config_t *)0)->embedding_command), 0, CFG_STRING},
    {"autonomous", offsetof(config_t, autonomous), sizeof(int), 1, CFG_BOOL},
    {"cross_verify", offsetof(config_t, cross_verify), sizeof(int), 1, CFG_BOOL},
    {"max_iterations", offsetof(config_t, max_iterations), sizeof(int), 0, CFG_INT},
    {"max_iterations_delegate", offsetof(config_t, max_iterations_delegate), sizeof(int), 0,
     CFG_INT},
    {NULL, 0, 0, 0, CFG_STRING},
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
      else if (f->type == CFG_INT)
      {
         int val = *(int *)((char *)&cfg + f->offset);
         printf("%d\n", val);
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
      else if (f->type == CFG_INT)
      {
         int *ptr = (int *)((char *)&cfg + f->offset);
         *ptr = atoi(value);
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

/* --- db subcmds (moved from cmd_core.c) --- */

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
