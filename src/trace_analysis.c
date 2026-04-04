/* trace_analysis.c: mine execution traces for recurring patterns */
#include "aimee.h"
#include "memory.h"
#include "trace_analysis.h"

#define RETRY_THRESHOLD    3   /* min repeated calls to flag as retry loop */
#define SEQUENCE_THRESHOLD 0.6 /* fraction for common sequence detection */
#define MAX_TRACES         512
#define MAX_SEQUENCES      128

/* A single trace row we care about */
typedef struct
{
   int64_t id;
   int plan_id;
   int turn;
   char direction[16];
   char tool_name[64];
   char tool_args[512];
   int has_error; /* 1 if tool_result contains error indicators */
} trace_row_t;

/* A tool pair for common sequence detection */
typedef struct
{
   char tool_a[64];
   char tool_b[64];
   int count;
   int total_plans;
} tool_pair_t;

/* Check if a tool result looks like an error */
static int result_looks_like_error(const char *result)
{
   if (!result || !*result)
      return 0;
   /* Common error indicators in tool results */
   if (strstr(result, "error") || strstr(result, "Error") || strstr(result, "ERROR"))
      return 1;
   if (strstr(result, "failed") || strstr(result, "Failed") || strstr(result, "FAILED"))
      return 1;
   if (strstr(result, "No such file") || strstr(result, "not found"))
      return 1;
   if (strstr(result, "Permission denied") || strstr(result, "command not found"))
      return 1;
   return 0;
}

/* Get the last mined trace ID so we only process new data */
static int64_t get_last_mined_id(sqlite3 *db)
{
   static const char *sql = "SELECT COALESCE(MAX(last_trace_id), 0)"
                            " FROM trace_mining_log";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return 0;

   int64_t last_id = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      last_id = sqlite3_column_int64(stmt, 0);
   sqlite3_reset(stmt);
   return last_id;
}

/* Record that we mined up to a given trace ID */
static void record_mining_run(sqlite3 *db, int64_t last_trace_id)
{
   static const char *sql = "INSERT INTO trace_mining_log (last_trace_id, mined_at)"
                            " VALUES (?, datetime('now'))";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return;

   sqlite3_bind_int64(stmt, 1, last_trace_id);
   DB_STEP_LOG(stmt, "record_mining_run");
   sqlite3_reset(stmt);
}

/* Load trace rows newer than last_mined_id */
static int load_traces(sqlite3 *db, int64_t after_id, trace_row_t *out, int max)
{
   static const char *sql = "SELECT id, COALESCE(plan_id, 0), turn, direction,"
                            " COALESCE(tool_name, ''), COALESCE(tool_args, ''),"
                            " COALESCE(tool_result, '')"
                            " FROM execution_trace"
                            " WHERE id > ? AND tool_name IS NOT NULL"
                            " ORDER BY plan_id, turn";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return 0;

   sqlite3_bind_int64(stmt, 1, after_id);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max)
   {
      trace_row_t *r = &out[count];
      r->id = sqlite3_column_int64(stmt, 0);
      r->plan_id = sqlite3_column_int(stmt, 1);
      r->turn = sqlite3_column_int(stmt, 2);
      snprintf(r->direction, sizeof(r->direction), "%s",
               (const char *)sqlite3_column_text(stmt, 3));
      snprintf(r->tool_name, sizeof(r->tool_name), "%s",
               (const char *)sqlite3_column_text(stmt, 4));
      snprintf(r->tool_args, sizeof(r->tool_args), "%s",
               (const char *)sqlite3_column_text(stmt, 5));
      r->has_error = result_looks_like_error((const char *)sqlite3_column_text(stmt, 6));
      count++;
   }
   sqlite3_reset(stmt);
   return count;
}

/* Check if an anti-pattern with this exact pattern text already exists */
static int anti_pattern_exists(sqlite3 *db, const char *pattern)
{
   static const char *sql = "SELECT COUNT(*) FROM anti_patterns WHERE pattern = ?";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return 0;

   sqlite3_bind_text(stmt, 1, pattern, -1, SQLITE_TRANSIENT);
   int exists = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      exists = sqlite3_column_int(stmt, 0) > 0;
   sqlite3_reset(stmt);
   return exists;
}

/* Check if a memory with this exact key already exists */
static int memory_key_exists(sqlite3 *db, const char *key)
{
   static const char *sql = "SELECT COUNT(*) FROM memories WHERE key = ?";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return 0;

   sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
   int exists = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      exists = sqlite3_column_int(stmt, 0) > 0;
   sqlite3_reset(stmt);
   return exists;
}

/* Detect retry loops: same tool with similar args called 3+ times with failures */
static int detect_retry_loops(sqlite3 *db, trace_row_t *traces, int count)
{
   int patterns = 0;

   for (int i = 0; i < count; i++)
   {
      if (!traces[i].tool_name[0])
         continue;

      /* Count consecutive calls to the same tool on the same plan */
      int run = 1;
      int errors = traces[i].has_error ? 1 : 0;

      for (int j = i + 1; j < count; j++)
      {
         if (traces[j].plan_id != traces[i].plan_id)
            break;
         if (strcmp(traces[j].tool_name, traces[i].tool_name) != 0)
            break;
         run++;
         if (traces[j].has_error)
            errors++;
      }

      if (run >= RETRY_THRESHOLD && errors >= 2)
      {
         char pattern[512];
         snprintf(pattern, sizeof(pattern), "Retry loop: %s called %d times with %d errors",
                  traces[i].tool_name, run, errors);

         if (!anti_pattern_exists(db, pattern))
         {
            char desc[1024];
            snprintf(desc, sizeof(desc),
                     "Tool '%s' was called %d consecutive times with %d failures."
                     " Consider a different approach after 2 failures.",
                     traces[i].tool_name, run, errors);

            anti_pattern_insert(db, pattern, desc, "trace_mining", "", 0.7, NULL);
            patterns++;
         }

         /* Skip past this run */
         i += run - 1;
      }
   }

   return patterns;
}

/* Detect recovery sequences: tool A fails, then tool B succeeds on same plan */
static int detect_recovery_sequences(sqlite3 *db, trace_row_t *traces, int count)
{
   int patterns = 0;
   const char *sid = session_id();

   for (int i = 0; i + 1 < count; i++)
   {
      if (!traces[i].has_error || !traces[i].tool_name[0])
         continue;
      if (traces[i + 1].plan_id != traces[i].plan_id)
         continue;
      if (traces[i + 1].has_error || !traces[i + 1].tool_name[0])
         continue;
      if (strcmp(traces[i].tool_name, traces[i + 1].tool_name) == 0)
         continue;

      char key[512];
      snprintf(key, sizeof(key), "recovery:%s->%s", traces[i].tool_name, traces[i + 1].tool_name);

      if (!memory_key_exists(db, key))
      {
         char content[2048];
         snprintf(content, sizeof(content), "When '%s' fails, try '%s' as a recovery step.",
                  traces[i].tool_name, traces[i + 1].tool_name);

         memory_insert(db, TIER_L0, KIND_PROCEDURE, key, content, 0.7, sid, NULL);
         patterns++;
      }
   }

   return patterns;
}

/* Detect common sequences: tool A followed by tool B across multiple plans */
static int detect_common_sequences(sqlite3 *db, trace_row_t *traces, int count)
{
   int patterns = 0;
   const char *sid = session_id();

   /* Collect unique plan IDs */
   int plan_ids[MAX_TRACES];
   int num_plans = 0;
   for (int i = 0; i < count; i++)
   {
      if (traces[i].plan_id == 0)
         continue;
      int found = 0;
      for (int p = 0; p < num_plans; p++)
      {
         if (plan_ids[p] == traces[i].plan_id)
         {
            found = 1;
            break;
         }
      }
      if (!found && num_plans < MAX_TRACES)
         plan_ids[num_plans++] = traces[i].plan_id;
   }

   if (num_plans < 3) /* need at least 3 plans for meaningful statistics */
      return 0;

   /* Count tool pair occurrences across plans */
   tool_pair_t pairs[MAX_SEQUENCES];
   memset(pairs, 0, sizeof(pairs));
   int num_pairs = 0;

   for (int i = 0; i + 1 < count; i++)
   {
      if (!traces[i].tool_name[0] || !traces[i + 1].tool_name[0])
         continue;
      if (traces[i].plan_id != traces[i + 1].plan_id)
         continue;
      if (traces[i].plan_id == 0)
         continue;

      /* Find or create this pair */
      int found = -1;
      for (int p = 0; p < num_pairs; p++)
      {
         if (strcmp(pairs[p].tool_a, traces[i].tool_name) == 0 &&
             strcmp(pairs[p].tool_b, traces[i + 1].tool_name) == 0)
         {
            found = p;
            break;
         }
      }

      if (found >= 0)
      {
         /* Check if this plan is already counted for this pair */
         /* Simple approach: just increment count (counts occurrences, not unique plans) */
         pairs[found].count++;
      }
      else if (num_pairs < MAX_SEQUENCES)
      {
         snprintf(pairs[num_pairs].tool_a, sizeof(pairs[num_pairs].tool_a), "%s",
                  traces[i].tool_name);
         snprintf(pairs[num_pairs].tool_b, sizeof(pairs[num_pairs].tool_b), "%s",
                  traces[i + 1].tool_name);
         pairs[num_pairs].count = 1;
         pairs[num_pairs].total_plans = num_plans;
         num_pairs++;
      }
   }

   /* Check which pairs appear in enough plans */
   for (int p = 0; p < num_pairs; p++)
   {
      /* Count unique plans containing this pair */
      int plans_with_pair = 0;
      for (int pi = 0; pi < num_plans; pi++)
      {
         int pid = plan_ids[pi];
         for (int i = 0; i + 1 < count; i++)
         {
            if (traces[i].plan_id != pid)
               continue;
            if (traces[i + 1].plan_id != pid)
               continue;
            if (strcmp(traces[i].tool_name, pairs[p].tool_a) == 0 &&
                strcmp(traces[i + 1].tool_name, pairs[p].tool_b) == 0)
            {
               plans_with_pair++;
               break;
            }
         }
      }

      double ratio = (double)plans_with_pair / num_plans;
      if (ratio < SEQUENCE_THRESHOLD)
         continue;

      char key[512];
      snprintf(key, sizeof(key), "sequence:%s->%s", pairs[p].tool_a, pairs[p].tool_b);

      if (!memory_key_exists(db, key))
      {
         char content[2048];
         snprintf(content, sizeof(content),
                  "Common pattern: '%s' is typically followed by '%s'"
                  " (observed in %d/%d plans, %.0f%%).",
                  pairs[p].tool_a, pairs[p].tool_b, plans_with_pair, num_plans, ratio * 100);

         memory_insert(db, TIER_L0, KIND_PROCEDURE, key, content, 0.6 + ratio * 0.2, sid, NULL);
         patterns++;
      }
   }

   return patterns;
}

int trace_mine(sqlite3 *db)
{
   if (!db)
      return -1;

   int64_t last_id = get_last_mined_id(db);

   trace_row_t *traces = calloc(MAX_TRACES, sizeof(trace_row_t));
   if (!traces)
      return -1;

   int count = load_traces(db, last_id, traces, MAX_TRACES);
   if (count == 0)
   {
      free(traces);
      return 0;
   }

   int64_t max_id = traces[count - 1].id;

   int retry_patterns = detect_retry_loops(db, traces, count);
   int recovery_patterns = detect_recovery_sequences(db, traces, count);
   int sequence_patterns = detect_common_sequences(db, traces, count);

   free(traces);

   int total = retry_patterns + recovery_patterns + sequence_patterns;

   /* Record the mining run */
   record_mining_run(db, max_id);

   if (total > 0)
      fprintf(stderr, "Trace mining: %d patterns (%d retry, %d recovery, %d sequence).\n", total,
              retry_patterns, recovery_patterns, sequence_patterns);

   return total;
}
