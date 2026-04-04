/* cmd_memory.c: memory subsystem CLI (store, get, list, search, task, decide, checkpoint, link) */
#include "aimee.h"
#include "commands.h"
#include "cJSON.h"

/* File-scope config, loaded once by cmd_memory before dispatch */
static config_t s_mem_cfg;

/* --- memory subcommand handlers --- */

static void mem_store(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);
   const char *tier = opt_get(&opts, "tier");
   if (!tier)
      tier = TIER_L0;
   const char *kind = opt_get(&opts, "kind");
   if (!kind)
      kind = KIND_FACT;
   const char *session = opt_get(&opts, "session");
   if (!session)
      session = "";
   const char *key = opt_pos(&opts, 0);
   const char *content = opt_pos(&opts, 1);
   if (!content)
      content = "";

   if (!key)
      fatal("memory store requires a key");

   const char *workspace = opt_get(&opts, "workspace");

   memory_t mem;
   if (memory_insert(db, tier, kind, key, content, 1.0, session, &mem) != 0)
      fatal("failed to store memory (key=%s, tier=%s) — check stderr for details", key, tier);

   /* Apply explicit workspace tag if provided */
   if (workspace && workspace[0])
      memory_tag_workspace(db, mem.id, workspace);

   if (ctx->json_output)
      emit_json_ctx(memory_to_json(&mem), ctx->json_fields, ctx->response_profile);
}

static void mem_get(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   if (argc < 1)
      fatal("memory get requires an id");
   int64_t id = atoll(argv[0]);
   memory_t mem;
   if (memory_get(db, id, &mem) != 0)
      fatal("memory not found: %lld", (long long)id);
   if (ctx->json_output)
      emit_json_ctx(memory_to_json(&mem), ctx->json_fields, ctx->response_profile);
}

static void mem_delete(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   if (argc < 1)
      fatal("memory delete requires an id");
   int64_t id = atoll(argv[0]);
   if (memory_delete(db, id) != 0)
      fatal("failed to delete memory: %lld", (long long)id);
   if (ctx->json_output)
      emit_ok_ctx(ctx->json_fields, ctx->response_profile);
}

static void mem_list(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);
   const char *tier = opt_get(&opts, "tier");
   const char *kind = opt_get(&opts, "kind");
   int limit = opt_get_int(&opts, "limit", 50);
   int low_eff = opt_get_int(&opts, "low-effectiveness", 0);

   if (low_eff)
   {
      /* List memories with low effectiveness scores */
      static const char *sql = "SELECT id, tier, kind, key, content, confidence, use_count,"
                               " last_used_at, created_at, updated_at, source_session,"
                               " effectiveness"
                               " FROM memories"
                               " WHERE effectiveness IS NOT NULL AND effectiveness < ?"
                               " ORDER BY effectiveness ASC LIMIT ?";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (!stmt)
         return;
      sqlite3_bind_double(stmt, 1, EFFECTIVENESS_DEMOTE_THRESHOLD);
      sqlite3_bind_int(stmt, 2, limit);

      cJSON *arr = cJSON_CreateArray();
      while (sqlite3_step(stmt) == SQLITE_ROW)
      {
         cJSON *m = cJSON_CreateObject();
         cJSON_AddNumberToObject(m, "id", sqlite3_column_int64(stmt, 0));
         const char *t = (const char *)sqlite3_column_text(stmt, 1);
         const char *ki = (const char *)sqlite3_column_text(stmt, 2);
         const char *k = (const char *)sqlite3_column_text(stmt, 3);
         cJSON_AddStringToObject(m, "tier", t ? t : "");
         cJSON_AddStringToObject(m, "kind", ki ? ki : "");
         cJSON_AddStringToObject(m, "key", k ? k : "");
         cJSON_AddNumberToObject(m, "effectiveness", sqlite3_column_double(stmt, 11));
         cJSON_AddNumberToObject(m, "use_count", sqlite3_column_int(stmt, 6));
         cJSON_AddItemToArray(arr, m);
      }
      sqlite3_reset(stmt);
      if (ctx->json_output)
         emit_json_ctx(arr, ctx->json_fields, ctx->response_profile);
      else
         cJSON_Delete(arr);
      return;
   }

   memory_t mems[256];
   int count = memory_list(db, tier, kind, limit, mems, 256);
   if (ctx->json_output)
   {
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; i < count; i++)
         cJSON_AddItemToArray(arr, memory_to_json(&mems[i]));
      emit_json_ctx(arr, ctx->json_fields, ctx->response_profile);
   }
}

static void mem_search(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   if (argc < 1)
      fatal("memory search requires query terms");

   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);
   int limit = opt_get_int(&opts, "limit", 10);

   char *clusters[64];
   int cluster_count = 0;
   for (int i = 0; i < opts.pos_count && cluster_count < 64; i++)
      clusters[cluster_count++] = (char *)opts.positional[i];

   /* Build combined query string for fact search */
   char query_buf[2048];
   int qpos = 0;
   for (int i = 0; i < cluster_count; i++)
   {
      if (i > 0)
         qpos += snprintf(query_buf + qpos, sizeof(query_buf) - qpos, " ");
      qpos += snprintf(query_buf + qpos, sizeof(query_buf) - qpos, "%s", clusters[i]);
   }

   /* Search stored facts */
   memory_t facts[64];
   int fact_count = memory_find_facts(db, query_buf, limit, facts, 64);

   /* Search conversation windows */
   int max_results = limit < 64 ? limit : 64;
   search_result_t *results = calloc((size_t)max_results, sizeof(search_result_t));
   if (!results)
      fatal("out of memory");
   int win_count = memory_search(db, clusters, cluster_count, limit, results, max_results);

   if (ctx->json_output)
   {
      cJSON *obj = cJSON_CreateObject();

      cJSON *farr = cJSON_CreateArray();
      for (int i = 0; i < fact_count; i++)
         cJSON_AddItemToArray(farr, memory_to_json(&facts[i]));
      cJSON_AddItemToObject(obj, "facts", farr);

      cJSON *warr = cJSON_CreateArray();
      for (int i = 0; i < win_count; i++)
         cJSON_AddItemToArray(warr, search_result_to_json(&results[i]));
      cJSON_AddItemToObject(obj, "windows", warr);

      emit_json_ctx(obj, ctx->json_fields, ctx->response_profile);
   }
   free(results);
}

static void mem_stats(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)argc;
   (void)argv;
   memory_stats_t stats;
   memory_stats(db, &stats);
   if (ctx->json_output)
   {
      cJSON *j = cJSON_CreateObject();
      cJSON_AddNumberToObject(j, "total", stats.total);
      cJSON_AddNumberToObject(j, "conflicts", stats.conflicts);
      cJSON *tiers = cJSON_AddObjectToObject(j, "tiers");
      cJSON_AddNumberToObject(tiers, "L0", stats.tier_counts[0]);
      cJSON_AddNumberToObject(tiers, "L1", stats.tier_counts[1]);
      cJSON_AddNumberToObject(tiers, "L2", stats.tier_counts[2]);
      cJSON_AddNumberToObject(tiers, "L3", stats.tier_counts[3]);

      effectiveness_stats_t estats;
      if (memory_effectiveness_stats(db, &estats) == 0)
      {
         cJSON *eff = cJSON_AddObjectToObject(j, "effectiveness");
         cJSON_AddNumberToObject(eff, "avg_effectiveness", estats.avg_effectiveness);
         cJSON_AddNumberToObject(eff, "low_effectiveness", estats.low_effectiveness_count);
         cJSON_AddNumberToObject(eff, "high_impact", estats.high_impact_count);
         cJSON_AddNumberToObject(eff, "never_surfaced_l2", estats.never_surfaced_l2);
      }

      emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
   }
}

static void mem_scan(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)argc;
   (void)argv;
   char dirs[8][MAX_PATH_LEN];
   int dir_count = config_conversation_dirs(&s_mem_cfg, dirs, 8);
   memory_scan_conversations(db, dirs, dir_count);
   if (ctx->json_output)
      emit_ok_ctx(ctx->json_fields, ctx->response_profile);
}

static void mem_edges(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   if (argc < 1)
      fatal("memory edges requires an entity name");
   edge_t edges[128];
   int count = memory_query_edges(db, argv[0], edges, 128);
   if (ctx->json_output)
   {
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; i < count; i++)
      {
         cJSON *e = cJSON_CreateObject();
         cJSON_AddNumberToObject(e, "id", (double)edges[i].id);
         cJSON_AddStringToObject(e, "source", edges[i].source);
         cJSON_AddStringToObject(e, "relation", edges[i].relation);
         cJSON_AddStringToObject(e, "target", edges[i].target);
         cJSON_AddNumberToObject(e, "weight", edges[i].weight);
         cJSON_AddItemToArray(arr, e);
      }
      emit_json_ctx(arr, ctx->json_fields, ctx->response_profile);
   }
}

static void mem_compact(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)argc;
   (void)argv;
   int summary_count = 0, fact_count = 0;
   memory_compact_windows(db, &summary_count, &fact_count);
   if (ctx->json_output)
   {
      cJSON *j = cJSON_CreateObject();
      cJSON_AddStringToObject(j, "status", "ok");
      cJSON_AddNumberToObject(j, "summaries", summary_count);
      cJSON_AddNumberToObject(j, "facts", fact_count);
      emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
   }
}

static void mem_conflicts(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)argc;
   (void)argv;
   conflict_t conflicts[64];
   int count = memory_list_conflicts(db, conflicts, 64);
   if (ctx->json_output)
   {
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; i < count; i++)
         cJSON_AddItemToArray(arr, conflict_to_json(&conflicts[i]));
      emit_json_ctx(arr, ctx->json_fields, ctx->response_profile);
   }
}

static void mem_health(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)argc;
   (void)argv;
   memory_health_t health;
   memory_query_health(db, &health);
   if (ctx->json_output)
   {
      cJSON *j = cJSON_CreateObject();
      cJSON_AddNumberToObject(j, "cycles", health.cycles);
      cJSON_AddNumberToObject(j, "contradiction_rate", health.contradiction_rate);
      cJSON_AddNumberToObject(j, "promotion_rate", health.promotion_rate);
      cJSON_AddNumberToObject(j, "demotion_rate", health.demotion_rate);
      cJSON_AddNumberToObject(j, "staleness", health.staleness);
      cJSON_AddNumberToObject(j, "total_contradictions", health.total_contradictions);
      cJSON_AddNumberToObject(j, "total_promotions", health.total_promotions);
      cJSON_AddNumberToObject(j, "total_demotions", health.total_demotions);
      cJSON_AddNumberToObject(j, "total_expirations", health.total_expirations);
      emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      printf("Memory Health (last 7 days, %d cycles):\n", health.cycles);
      printf("  Contradiction rate: %.1f%% (%d detected)\n", health.contradiction_rate * 100,
             health.total_contradictions);
      printf("  Promotion rate:     %.1f%% (%d promoted)\n", health.promotion_rate * 100,
             health.total_promotions);
      printf("  Demotion rate:      %.1f%% (%d demoted)\n", health.demotion_rate * 100,
             health.total_demotions);
      printf("  Staleness:          %.1f%% of L2 facts unused in 30+ days\n",
             health.staleness * 100);
      printf("  Expirations:        %d\n", health.total_expirations);
   }
}

static void mem_provenance(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   /* --stale flag: show memories with suspicious provenance */
   if (argc >= 1 && strcmp(argv[0], "--stale") == 0)
   {
      if (ctx->json_output)
      {
         cJSON *root = cJSON_CreateObject();

         /* Never-used L2 memories older than 14 days */
         cJSON *unused = cJSON_CreateArray();
         {
            static const char *sql = "SELECT id, key, tier, kind, confidence FROM memories"
                                     " WHERE tier = 'L2' AND use_count = 0"
                                     " AND created_at < datetime('now', '-14 days')"
                                     " ORDER BY created_at ASC";
            sqlite3_stmt *stmt = db_prepare(db, sql);
            if (stmt)
            {
               while (sqlite3_step(stmt) == SQLITE_ROW)
               {
                  cJSON *m = cJSON_CreateObject();
                  cJSON_AddNumberToObject(m, "id", sqlite3_column_int64(stmt, 0));
                  const char *k = (const char *)sqlite3_column_text(stmt, 1);
                  cJSON_AddStringToObject(m, "key", k ? k : "");
                  cJSON_AddItemToArray(unused, m);
               }
               sqlite3_reset(stmt);
            }
         }
         cJSON_AddItemToObject(root, "never_used", unused);

         /* Frequently superseded keys (3+ versions) */
         cJSON *superseded = cJSON_CreateArray();
         {
            static const char *sql =
                "SELECT SUBSTR(key, 1, INSTR(key, '#v') - 1) AS base_key, COUNT(*) AS versions"
                " FROM memories WHERE key LIKE '%#v%'"
                " GROUP BY base_key HAVING versions >= 3"
                " ORDER BY versions DESC";
            sqlite3_stmt *stmt = db_prepare(db, sql);
            if (stmt)
            {
               while (sqlite3_step(stmt) == SQLITE_ROW)
               {
                  cJSON *s = cJSON_CreateObject();
                  const char *k = (const char *)sqlite3_column_text(stmt, 0);
                  cJSON_AddStringToObject(s, "base_key", k ? k : "");
                  cJSON_AddNumberToObject(s, "versions", sqlite3_column_int(stmt, 1));
                  cJSON_AddItemToArray(superseded, s);
               }
               sqlite3_reset(stmt);
            }
         }
         cJSON_AddItemToObject(root, "frequently_superseded", superseded);

         emit_json_ctx(root, ctx->json_fields, ctx->response_profile);
      }
      else
      {
         /* Never-used L2 memories */
         printf("Never-used L2 memories (>14 days old):\n");
         {
            static const char *sql = "SELECT id, key FROM memories"
                                     " WHERE tier = 'L2' AND use_count = 0"
                                     " AND created_at < datetime('now', '-14 days')"
                                     " ORDER BY created_at ASC";
            sqlite3_stmt *stmt = db_prepare(db, sql);
            int found = 0;
            if (stmt)
            {
               while (sqlite3_step(stmt) == SQLITE_ROW)
               {
                  printf("  #%-6lld %s\n", (long long)sqlite3_column_int64(stmt, 0),
                         (const char *)sqlite3_column_text(stmt, 1));
                  found++;
               }
               sqlite3_reset(stmt);
            }
            if (!found)
               printf("  (none)\n");
         }

         /* Frequently superseded */
         printf("\nFrequently superseded keys (3+ versions):\n");
         {
            static const char *sql =
                "SELECT SUBSTR(key, 1, INSTR(key, '#v') - 1) AS base_key, COUNT(*) AS versions"
                " FROM memories WHERE key LIKE '%#v%'"
                " GROUP BY base_key HAVING versions >= 3"
                " ORDER BY versions DESC";
            sqlite3_stmt *stmt = db_prepare(db, sql);
            int found = 0;
            if (stmt)
            {
               while (sqlite3_step(stmt) == SQLITE_ROW)
               {
                  printf("  %-40s %d versions\n", (const char *)sqlite3_column_text(stmt, 0),
                         sqlite3_column_int(stmt, 1));
                  found++;
               }
               sqlite3_reset(stmt);
            }
            if (!found)
               printf("  (none)\n");
         }
      }
      return;
   }

   /* Default: show provenance for a specific memory ID */
   if (argc < 1)
      fatal("usage: aimee memory provenance <id> | --stale");

   int64_t id = atoll(argv[0]);

   /* Get memory info */
   memory_t mem;
   if (memory_get(db, id, &mem) != 0)
      fatal("memory not found: %lld", (long long)id);

   provenance_entry_t entries[MAX_PROVENANCE_ENTRIES];
   int count = memory_get_provenance(db, id, entries, MAX_PROVENANCE_ENTRIES);

   if (ctx->json_output)
   {
      cJSON *root = cJSON_CreateObject();
      cJSON_AddNumberToObject(root, "memory_id", (double)id);
      cJSON_AddStringToObject(root, "key", mem.key);
      cJSON_AddStringToObject(root, "tier", mem.tier);
      cJSON_AddStringToObject(root, "kind", mem.kind);
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; i < count; i++)
      {
         cJSON *e = cJSON_CreateObject();
         cJSON_AddStringToObject(e, "created_at", entries[i].created_at);
         cJSON_AddStringToObject(e, "action", entries[i].action);
         cJSON_AddStringToObject(e, "session_id", entries[i].session_id);
         if (entries[i].details[0])
            cJSON_AddStringToObject(e, "details", entries[i].details);
         cJSON_AddItemToArray(arr, e);
      }
      cJSON_AddItemToObject(root, "provenance", arr);
      emit_json_ctx(root, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      printf("Memory #%lld: %s (%s, %s, confidence: %.2f)\n", (long long)id, mem.key, mem.tier,
             mem.kind, mem.confidence);
      if (count == 0)
      {
         printf("  (no provenance records)\n");
         return;
      }
      for (int i = 0; i < count; i++)
      {
         /* Show date part only (first 10 chars of ISO 8601) */
         char date[11] = {0};
         snprintf(date, sizeof(date), "%.10s", entries[i].created_at);
         printf("  %s  %-10s session:%.8s", date, entries[i].action, entries[i].session_id);
         if (entries[i].details[0])
            printf("  \"%s\"", entries[i].details);
         printf("\n");
      }
   }
}

static void mem_maintain(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)argc;
   (void)argv;
   int promoted = 0, demoted = 0, expired = 0;
   memory_run_maintenance(db, &promoted, &demoted, &expired);
   if (ctx->json_output)
   {
      cJSON *j = cJSON_CreateObject();
      cJSON_AddStringToObject(j, "status", "ok");
      cJSON_AddNumberToObject(j, "promoted", promoted);
      cJSON_AddNumberToObject(j, "demoted", demoted);
      cJSON_AddNumberToObject(j, "expired", expired);
      emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
   }
}

static void mem_task(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   if (argc < 1)
      fatal("memory task requires a subcommand: "
            "create, list, update, edges, link, delete");

   const char *tsub = argv[0];
   argc--;
   argv++;

   if (strcmp(tsub, "create") == 0)
   {
      if (argc < 1)
         fatal("memory task create requires a title");

      opt_parsed_t opts;
      opt_parse(argc, argv, NULL, &opts);
      const char *session = opt_get(&opts, "session");
      if (!session)
         session = "";
      const char *parent_str = opt_get(&opts, "parent");
      int64_t parent = parent_str ? atoll(parent_str) : 0;
      const char *title = opt_pos(&opts, 0);

      if (!title)
         fatal("memory task create requires a title");

      aimee_task_t task;
      if (aimee_task_create(db, title, session, parent, &task) != 0)
         fatal("failed to create task");
      if (ctx->json_output)
         emit_json_ctx(aimee_task_to_json(&task), ctx->json_fields, ctx->response_profile);
   }
   else if (strcmp(tsub, "list") == 0)
   {
      opt_parsed_t opts;
      opt_parse(argc, argv, NULL, &opts);
      const char *state = opt_get(&opts, "state");
      const char *session = opt_get(&opts, "session");
      int limit = opt_get_int(&opts, "limit", 50);

      aimee_task_t tasks[128];
      int count = aimee_task_list(db, state, session, limit, tasks, 128);
      if (ctx->json_output)
      {
         cJSON *arr = cJSON_CreateArray();
         for (int i = 0; i < count; i++)
            cJSON_AddItemToArray(arr, aimee_task_to_json(&tasks[i]));
         emit_json_ctx(arr, ctx->json_fields, ctx->response_profile);
      }
   }
   else if (strcmp(tsub, "update") == 0)
   {
      if (argc < 2)
         fatal("memory task update requires id and state");
      int64_t id = atoll(argv[0]);
      task_update_state(db, id, argv[1]);
      if (ctx->json_output)
         emit_ok_ctx(ctx->json_fields, ctx->response_profile);
   }
   else if (strcmp(tsub, "edges") == 0)
   {
      if (argc < 1)
         fatal("memory task edges requires a task id");
      int64_t id = atoll(argv[0]);
      task_edge_t edges[64];
      int count = task_get_edges(db, id, edges, 64);
      if (ctx->json_output)
      {
         cJSON *arr = cJSON_CreateArray();
         for (int i = 0; i < count; i++)
         {
            cJSON *e = cJSON_CreateObject();
            cJSON_AddNumberToObject(e, "id", (double)edges[i].id);
            cJSON_AddNumberToObject(e, "source_id", (double)edges[i].source_id);
            cJSON_AddNumberToObject(e, "target_id", (double)edges[i].target_id);
            cJSON_AddStringToObject(e, "relation", edges[i].relation);
            cJSON_AddItemToArray(arr, e);
         }
         emit_json_ctx(arr, ctx->json_fields, ctx->response_profile);
      }
   }
   else if (strcmp(tsub, "link") == 0)
   {
      if (argc < 2)
         fatal("memory task link requires source and target ids");
      int64_t source = atoll(argv[0]);
      int64_t target = atoll(argv[1]);
      const char *relation = "depends_on";
      if (argc >= 3)
         relation = argv[2];
      task_add_edge(db, source, target, relation);
      if (ctx->json_output)
         emit_ok_ctx(ctx->json_fields, ctx->response_profile);
   }
   else if (strcmp(tsub, "delete") == 0)
   {
      if (argc < 1)
         fatal("memory task delete requires an id");
      int64_t id = atoll(argv[0]);
      aimee_task_delete(db, id);
      if (ctx->json_output)
         emit_ok_ctx(ctx->json_fields, ctx->response_profile);
   }
   else
   {
      fatal("unknown task subcommand: %s", tsub);
   }
}

static void mem_decide(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);
   const char *options = opt_get(&opts, "options");
   if (!options)
      options = "";
   const char *chosen = opt_get(&opts, "chosen");
   if (!chosen)
      chosen = "";
   const char *rationale = opt_get(&opts, "rationale");
   if (!rationale)
      rationale = "";
   const char *assumptions = opt_get(&opts, "assumptions");
   if (!assumptions)
      assumptions = "";
   const char *task_str = opt_get(&opts, "task");
   int64_t task_id = task_str ? atoll(task_str) : 0;

   decision_t dec;
   if (decision_log(db, options, chosen, rationale, assumptions, task_id, &dec) != 0)
      fatal("failed to log decision");
   if (ctx->json_output)
      emit_json_ctx(decision_to_json(&dec), ctx->json_fields, ctx->response_profile);
}

static void mem_decisions(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);
   const char *outcome = opt_get(&opts, "outcome");
   int limit = opt_get_int(&opts, "limit", 50);

   decision_t decs[128];
   int count = decision_list(db, outcome, limit, decs, 128);
   if (ctx->json_output)
   {
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; i < count; i++)
         cJSON_AddItemToArray(arr, decision_to_json(&decs[i]));
      emit_json_ctx(arr, ctx->json_fields, ctx->response_profile);
   }
}

static void mem_antipattern(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   if (argc < 1)
      fatal("memory antipattern requires: list, add, or delete");

   const char *apsub = argv[0];
   argc--;
   argv++;

   if (strcmp(apsub, "list") == 0)
   {
      anti_pattern_t aps[128];
      int count = anti_pattern_list(db, aps, 128);
      if (ctx->json_output)
      {
         cJSON *arr = cJSON_CreateArray();
         for (int i = 0; i < count; i++)
            cJSON_AddItemToArray(arr, anti_pattern_to_json(&aps[i]));
         emit_json_ctx(arr, ctx->json_fields, ctx->response_profile);
      }
   }
   else if (strcmp(apsub, "add") == 0)
   {
      opt_parsed_t opts;
      opt_parse(argc, argv, NULL, &opts);
      const char *desc = opt_get(&opts, "desc");
      if (!desc)
         desc = "";
      const char *source = opt_get(&opts, "source");
      if (!source)
         source = "";
      const char *ref = opt_get(&opts, "ref");
      if (!ref)
         ref = "";
      const char *conf_str = opt_get(&opts, "confidence");
      double conf = conf_str ? atof(conf_str) : 1.0;
      const char *pattern = opt_pos(&opts, 0);

      if (!pattern)
         fatal("memory antipattern add requires a pattern");

      anti_pattern_t ap;
      if (anti_pattern_insert(db, pattern, desc, source, ref, conf, &ap) != 0)
         fatal("failed to add anti-pattern");
      if (ctx->json_output)
         emit_json_ctx(anti_pattern_to_json(&ap), ctx->json_fields, ctx->response_profile);
   }
   else if (strcmp(apsub, "delete") == 0)
   {
      if (argc < 1)
         fatal("memory antipattern delete requires an id");
      int64_t id = atoll(argv[0]);
      anti_pattern_delete(db, id);
      if (ctx->json_output)
         emit_ok_ctx(ctx->json_fields, ctx->response_profile);
   }
   else
   {
      fatal("unknown antipattern subcommand: %s", apsub);
   }
}

static void mem_supersede(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   if (argc < 2)
      fatal("memory supersede requires old_id and new_content");

   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);
   const char *id_str = opt_pos(&opts, 0);
   const char *new_content = opt_pos(&opts, 1);
   if (!id_str || !new_content)
      fatal("memory supersede requires old_id and new_content");
   int64_t old_id = atoll(id_str);
   const char *conf_str = opt_get(&opts, "confidence");
   double conf = conf_str ? atof(conf_str) : 1.0;
   const char *session = opt_get(&opts, "session");
   if (!session)
      session = "";

   memory_t mem;
   if (memory_supersede(db, old_id, new_content, conf, session, &mem) != 0)
      fatal("failed to supersede memory");
   if (ctx->json_output)
      emit_json_ctx(memory_to_json(&mem), ctx->json_fields, ctx->response_profile);
}

static void mem_history(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   if (argc < 1)
      fatal("memory history requires a key");
   memory_t mems[64];
   int count = memory_fact_history(db, argv[0], mems, 64);
   if (ctx->json_output)
   {
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; i < count; i++)
         cJSON_AddItemToArray(arr, memory_to_json(&mems[i]));
      emit_json_ctx(arr, ctx->json_fields, ctx->response_profile);
   }
}

static void mem_checkpoint(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   if (argc < 1)
      fatal("memory checkpoint requires: create, list, restore, delete");

   const char *cpsub = argv[0];
   argc--;
   argv++;

   if (strcmp(cpsub, "create") == 0)
   {
      opt_parsed_t opts;
      opt_parse(argc, argv, NULL, &opts);
      const char *session = opt_get(&opts, "session");
      if (!session)
         session = "";
      const char *task_str = opt_get(&opts, "task");
      int64_t task_id = task_str ? atoll(task_str) : 0;
      const char *label = opt_pos(&opts, 0);
      if (!label)
         label = "";

      checkpoint_t cp;
      if (checkpoint_create(db, label, session, task_id, &cp) != 0)
         fatal("failed to create checkpoint");
      if (ctx->json_output)
         emit_json_ctx(checkpoint_to_json(&cp), ctx->json_fields, ctx->response_profile);
   }
   else if (strcmp(cpsub, "list") == 0)
   {
      opt_parsed_t opts;
      opt_parse(argc, argv, NULL, &opts);
      int limit = opt_get_int(&opts, "limit", 50);

      checkpoint_t cps[64];
      int count = checkpoint_list(db, limit, cps, 64);
      if (ctx->json_output)
      {
         cJSON *arr = cJSON_CreateArray();
         for (int i = 0; i < count; i++)
            cJSON_AddItemToArray(arr, checkpoint_to_json(&cps[i]));
         emit_json_ctx(arr, ctx->json_fields, ctx->response_profile);
      }
   }
   else if (strcmp(cpsub, "restore") == 0)
   {
      if (argc < 1)
         fatal("memory checkpoint restore requires an id");
      opt_parsed_t rsopts;
      opt_parse(argc, argv, NULL, &rsopts);
      const char *id_str = opt_pos(&rsopts, 0);
      if (!id_str)
         fatal("memory checkpoint restore requires an id");
      int64_t id = atoll(id_str);
      const char *session = opt_get(&rsopts, "session");
      if (!session)
         session = "";
      checkpoint_restore(db, id, session);
      if (ctx->json_output)
         emit_ok_ctx(ctx->json_fields, ctx->response_profile);
   }
   else if (strcmp(cpsub, "delete") == 0)
   {
      if (argc < 1)
         fatal("memory checkpoint delete requires an id");
      int64_t id = atoll(argv[0]);
      checkpoint_delete(db, id);
      if (ctx->json_output)
         emit_ok_ctx(ctx->json_fields, ctx->response_profile);
   }
   else
   {
      fatal("unknown checkpoint subcommand: %s", cpsub);
   }
}

static void mem_style(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)argc;
   (void)argv;
   memory_learn_style(db);
   if (ctx->json_output)
      emit_ok_ctx(ctx->json_fields, ctx->response_profile);
}

static void mem_read(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)argc;
   (void)argv;
   /* Read = assemble context */
   char *mem_ctx = memory_assemble_context(db, NULL);
   if (ctx->json_output)
   {
      cJSON *j = cJSON_CreateObject();
      cJSON_AddStringToObject(j, "context", mem_ctx ? mem_ctx : "");
      emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
   }
   free(mem_ctx);
}

static void mem_link(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   /* Artifact-aware memory: link a memory to an artifact (Feature 9) */
   if (argc < 3)
      fatal("usage: aimee memory link <id> file|commit|pr <ref>");
   int mem_id = atoi(argv[0]);
   const char *artifact_type = argv[1];
   const char *artifact_ref = argv[2];

   /* Validate artifact type */
   if (strcmp(artifact_type, "file") != 0 && strcmp(artifact_type, "commit") != 0 &&
       strcmp(artifact_type, "pr") != 0 && strcmp(artifact_type, "test_run") != 0)
      fatal("artifact type must be: file, commit, pr, or test_run");

   /* Compute hash for files */
   char hash[65] = {0};
   if (strcmp(artifact_type, "file") == 0)
   {
      FILE *f = fopen(artifact_ref, "r");
      if (f)
      {
         /* Simple hash: sum of bytes mod large prime */
         unsigned long h = 0;
         int c;
         while ((c = fgetc(f)) != EOF)
            h = h * 31 + (unsigned long)c;
         fclose(f);
         snprintf(hash, sizeof(hash), "%016lx", h);
      }
   }

   static const char *sql = "UPDATE memories SET artifact_type = ?, artifact_ref = ?,"
                            " artifact_hash = ? WHERE id = ?";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (stmt)
   {
      sqlite3_reset(stmt);
      sqlite3_bind_text(stmt, 1, artifact_type, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 2, artifact_ref, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 3, hash[0] ? hash : NULL, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(stmt, 4, mem_id);
      if (sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) > 0)
      {
         if (ctx->json_output)
         {
            cJSON *j = cJSON_CreateObject();
            cJSON_AddStringToObject(j, "status", "ok");
            cJSON_AddNumberToObject(j, "memory_id", mem_id);
            cJSON_AddStringToObject(j, "artifact_type", artifact_type);
            cJSON_AddStringToObject(j, "artifact_ref", artifact_ref);
            if (hash[0])
               cJSON_AddStringToObject(j, "artifact_hash", hash);
            emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
         }
         else
         {
            printf("Linked memory %d to %s:%s\n", mem_id, artifact_type, artifact_ref);
         }
      }
      else
      {
         fatal("memory %d not found", mem_id);
      }
   }
}

static void mem_mlink(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   if (argc < 3)
      fatal("usage: aimee memory mlink <source_id> <target_id> <relation>");
   int64_t src = atoll(argv[0]);
   int64_t tgt = atoll(argv[1]);
   const char *rel = argv[2];

   if (strcmp(rel, "supersedes") != 0 && strcmp(rel, "depends_on") != 0 &&
       strcmp(rel, "contradicts") != 0 && strcmp(rel, "related_to") != 0)
      fatal("relation must be: supersedes, depends_on, contradicts, or related_to");

   if (memory_link_create(db, src, tgt, rel) != 0)
      fatal("failed to create link");

   if (ctx->json_output)
      emit_ok_ctx(ctx->json_fields, ctx->response_profile);
   else
      printf("Linked memory %lld -[%s]-> %lld\n", (long long)src, rel, (long long)tgt);
}

static void mem_mlinks(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   if (argc < 1)
      fatal("usage: aimee memory mlinks <id>");
   int64_t id = atoll(argv[0]);

   memory_link_t links[32];
   int count = memory_link_query(db, id, links, 32);

   if (ctx->json_output)
   {
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; i < count; i++)
      {
         cJSON *obj = cJSON_CreateObject();
         cJSON_AddNumberToObject(obj, "id", (double)links[i].id);
         cJSON_AddNumberToObject(obj, "source_id", (double)links[i].source_id);
         cJSON_AddNumberToObject(obj, "target_id", (double)links[i].target_id);
         cJSON_AddStringToObject(obj, "relation", links[i].relation);
         cJSON_AddStringToObject(obj, "created_at", links[i].created_at);
         cJSON_AddItemToArray(arr, obj);
      }
      char *json = cJSON_Print(arr);
      printf("%s\n", json);
      free(json);
      cJSON_Delete(arr);
   }
   else
   {
      if (count == 0)
      {
         printf("No links for memory %lld\n", (long long)id);
         return;
      }
      for (int i = 0; i < count; i++)
      {
         const char *dir = (links[i].source_id == id) ? "->" : "<-";
         int64_t other = (links[i].source_id == id) ? links[i].target_id : links[i].source_id;
         printf("  [%lld] %s [%s] %lld  (%s)\n", (long long)links[i].id, dir, links[i].relation,
                (long long)other, links[i].created_at);
      }
   }
}

static void mem_munlink(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   if (argc < 1)
      fatal("usage: aimee memory munlink <link_id>");
   int64_t link_id = atoll(argv[0]);

   if (memory_link_delete(db, link_id) != 0)
      fatal("failed to delete link %lld", (long long)link_id);

   if (ctx->json_output)
      emit_ok_ctx(ctx->json_fields, ctx->response_profile);
   else
      printf("Deleted link %lld\n", (long long)link_id);
}

static void mem_tag(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   if (argc < 2)
      fatal("usage: aimee memory tag <id> <workspace>");
   int64_t id = atoll(argv[0]);
   const char *workspace = argv[1];
   if (memory_tag_workspace(db, id, workspace) != 0)
      fatal("failed to tag memory %lld", (long long)id);
   if (ctx->json_output)
      emit_ok_ctx(ctx->json_fields, ctx->response_profile);
   else
      printf("Tagged memory %lld with workspace '%s'\n", (long long)id, workspace);
}

static void mem_embed(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   if (!s_mem_cfg.embedding_command[0])
      fatal("no embedding_command configured in config.json");

   int all = 0;
   int64_t single_id = 0;
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--all") == 0)
         all = 1;
      else
         single_id = atoll(argv[i]);
   }

   if (!all && single_id <= 0)
      fatal("usage: aimee memory embed --all  OR  aimee memory embed <id>");

   if (single_id > 0)
   {
      int rc = memory_embed(db, single_id, s_mem_cfg.embedding_command);
      if (rc != 0)
         fatal("failed to embed memory %lld", (long long)single_id);
      printf("Embedded memory %lld\n", (long long)single_id);
      return;
   }

   /* Batch: embed all L1/L2 memories without embeddings */
   static const char *sql = "SELECT m.id FROM memories m"
                            " LEFT JOIN memory_embeddings e ON e.memory_id = m.id"
                            " WHERE m.tier IN ('L1', 'L2') AND e.memory_id IS NULL";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return;

   int64_t ids[1024];
   int id_count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && id_count < 1024)
      ids[id_count++] = sqlite3_column_int64(stmt, 0);
   sqlite3_reset(stmt);

   int success = 0, fail = 0;
   for (int i = 0; i < id_count; i++)
   {
      int rc = memory_embed(db, ids[i], s_mem_cfg.embedding_command);
      if (rc == 0)
         success++;
      else
         fail++;
   }

   if (ctx->json_output)
   {
      cJSON *j = cJSON_CreateObject();
      cJSON_AddNumberToObject(j, "embedded", success);
      cJSON_AddNumberToObject(j, "failed", fail);
      emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      printf("Embedded %d memories (%d failed)\n", success, fail);
   }
}

/* --- memory subcommand table --- */

static const subcmd_t memory_subcmds[] = {
    {"store", "Store a memory (key, content, --tier, --kind)", mem_store},
    {"get", "Retrieve a memory by key", mem_get},
    {"delete", "Delete a memory by ID", mem_delete},
    {"list", "List memories (--tier, --kind, --limit)", mem_list},
    {"search", "Full-text search across all memories", mem_search},
    {"stats", "Show memory tier statistics", mem_stats},
    {"scan", "Scan conversations for learnable patterns", mem_scan},
    {"edges", "Show memory graph edges", mem_edges},
    {"compact", "Compact old memory windows", mem_compact},
    {"conflicts", "Show conflicting memories", mem_conflicts},
    {"maintain", "Run promotion/demotion/expiry cycle", mem_maintain},
    {"health", "Show memory system health metrics (7-day rolling)", mem_health},
    {"provenance", "Show provenance chain for a memory (or --stale)", mem_provenance},
    {"task", "Create or update a task memory", mem_task},
    {"decide", "Record a decision", mem_decide},
    {"decisions", "List recorded decisions", mem_decisions},
    {"antipattern", "Record an anti-pattern", mem_antipattern},
    {"supersede", "Supersede an old memory with a new one", mem_supersede},
    {"history", "Show memory mutation history", mem_history},
    {"checkpoint", "Save a session checkpoint", mem_checkpoint},
    {"style", "Show learned style preferences", mem_style},
    {"read", "Read a memory's full content", mem_read},
    {"link", "Link a memory to an artifact (file, commit, pr)", mem_link},
    {"mlink", "Create a memory-to-memory link (depends_on, etc.)", mem_mlink},
    {"mlinks", "Show memory-to-memory links for a given ID", mem_mlinks},
    {"munlink", "Remove a memory-to-memory link", mem_munlink},
    {"tag", "Tag a memory with a workspace scope", mem_tag},
    {"embed", "Generate embeddings for memories (--all or <id>)", mem_embed},
    {NULL, NULL, NULL},
};

const subcmd_t *get_memory_subcmds(void)
{
   return memory_subcmds;
}

/* --- cmd_memory --- */

void cmd_memory(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
   {
      subcmd_usage("memory", memory_subcmds);
      exit(1);
   }

   const char *sub = argv[0];
   argc--;
   argv++;

   sqlite3 *db = ctx_db_open_fast(ctx);
   if (!db)
      fatal("cannot open database");

   config_load(&s_mem_cfg);

   if (subcmd_dispatch(memory_subcmds, sub, ctx, db, argc, argv) != 0)
      fatal("unknown memory subcommand: %s", sub);

   ctx_db_close(ctx);
}
