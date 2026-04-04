/* memory_promote.c: promotion, demotion, expiry, maintenance, fold, conflict */
#include "aimee.h"
#include <time.h>
#include <math.h>
#include <strings.h>

/* --- Kind Lifecycle Configuration --- */

/* Default lifecycle for unknown kinds (matches original fact thresholds) */
static const kind_lifecycle_t default_lifecycle = {
    .promote_use_count = PROMOTE_L1_USE_COUNT,
    .promote_confidence = PROMOTE_L1_CONFIDENCE,
    .demote_days = DEMOTE_L2_DAYS,
    .demote_confidence = DEMOTE_L2_CONFIDENCE,
    .expire_days = EXPIRE_L1_DAYS,
    .demotion_resistance = 1.0,
};

int kind_lifecycle_load(sqlite3 *db, const char *kind, kind_lifecycle_t *out)
{
   *out = default_lifecycle;
   if (!db || !kind)
      return -1;

   static const char *sql = "SELECT promote_use_count, promote_confidence,"
                            " demote_days, demote_confidence, expire_days,"
                            " demotion_resistance"
                            " FROM kind_lifecycle WHERE kind = ?";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return -1;

   sqlite3_bind_text(stmt, 1, kind, -1, SQLITE_TRANSIENT);
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      out->promote_use_count = sqlite3_column_int(stmt, 0);
      out->promote_confidence = sqlite3_column_double(stmt, 1);
      out->demote_days = sqlite3_column_int(stmt, 2);
      out->demote_confidence = sqlite3_column_double(stmt, 3);
      out->expire_days = sqlite3_column_int(stmt, 4);
      out->demotion_resistance = sqlite3_column_double(stmt, 5);
      sqlite3_reset(stmt);
      return 0;
   }
   sqlite3_reset(stmt);
   return -1; /* kind not found, defaults applied */
}

/* --- Promotion / Demotion / Expiry --- */

/* Promote L1 -> L2 per kind-specific thresholds */
int memory_promote(sqlite3 *db)
{
   char ts[32];
   now_utc(ts, sizeof(ts));
   int total = 0;

   /* Get distinct kinds present at L1 */
   static const char *kinds_sql = "SELECT DISTINCT kind FROM memories WHERE tier = 'L1'";
   sqlite3_stmt *ks = db_prepare(db, kinds_sql);
   if (!ks)
      return 0;

   char kinds[16][16];
   int nkinds = 0;
   while (sqlite3_step(ks) == SQLITE_ROW && nkinds < 16)
   {
      const char *k = (const char *)sqlite3_column_text(ks, 0);
      if (k)
         snprintf(kinds[nkinds++], sizeof(kinds[0]), "%s", k);
   }
   sqlite3_reset(ks);

   /* Promote each kind with its own thresholds */
   static const char *sql = "UPDATE memories SET tier = 'L2', updated_at = ?"
                            " WHERE tier = 'L1' AND kind = ?"
                            " AND (use_count >= ? OR confidence >= ?)";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return 0;

   for (int i = 0; i < nkinds; i++)
   {
      kind_lifecycle_t lc;
      kind_lifecycle_load(db, kinds[i], &lc);

      sqlite3_bind_text(stmt, 1, ts, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 2, kinds[i], -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(stmt, 3, lc.promote_use_count);
      sqlite3_bind_double(stmt, 4, lc.promote_confidence);
      DB_STEP_LOG(stmt, "memory_promote");
      total += sqlite3_changes(db);
      sqlite3_reset(stmt);
   }

   return total;
}

/* Demote L2 -> L1 per kind-specific thresholds (with demotion resistance) */
int memory_demote(sqlite3 *db)
{
   char ts[32];
   now_utc(ts, sizeof(ts));
   int total = 0;

   /* Get distinct kinds present at L2 */
   static const char *kinds_sql = "SELECT DISTINCT kind FROM memories WHERE tier = 'L2'";
   sqlite3_stmt *ks = db_prepare(db, kinds_sql);
   if (!ks)
      return 0;

   char kinds[16][16];
   int nkinds = 0;
   while (sqlite3_step(ks) == SQLITE_ROW && nkinds < 16)
   {
      const char *k = (const char *)sqlite3_column_text(ks, 0);
      if (k)
         snprintf(kinds[nkinds++], sizeof(kinds[0]), "%s", k);
   }
   sqlite3_reset(ks);

   static const char *sql = "UPDATE memories SET tier = 'L1', updated_at = ?"
                            " WHERE tier = 'L2' AND kind = ?"
                            " AND confidence < ?"
                            " AND last_used_at < datetime('now', ? || ' days')";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return 0;

   for (int i = 0; i < nkinds; i++)
   {
      kind_lifecycle_t lc;
      kind_lifecycle_load(db, kinds[i], &lc);

      int effective_days = (int)(lc.demote_days * lc.demotion_resistance);
      char days_str[16];
      snprintf(days_str, sizeof(days_str), "-%d", effective_days);

      sqlite3_bind_text(stmt, 1, ts, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 2, kinds[i], -1, SQLITE_TRANSIENT);
      sqlite3_bind_double(stmt, 3, lc.demote_confidence);
      sqlite3_bind_text(stmt, 4, days_str, -1, SQLITE_TRANSIENT);
      DB_STEP_LOG(stmt, "memory_demote");
      total += sqlite3_changes(db);
      sqlite3_reset(stmt);
   }

   /* Cascade: reduce confidence of memories that depend_on recently demoted ones.
    * A demoted memory is now L1 with updated_at = ts (just set above). */
   if (total > 0)
   {
      static const char *cascade_sql = "UPDATE memories SET confidence = confidence * 0.9"
                                       " WHERE id IN ("
                                       "  SELECT ml.source_id FROM memory_links ml"
                                       "  JOIN memories m ON m.id = ml.target_id"
                                       "  WHERE ml.relation = 'depends_on'"
                                       "  AND m.tier = 'L1' AND m.updated_at = ?"
                                       ")";
      sqlite3_stmt *cs = db_prepare(db, cascade_sql);
      if (cs)
      {
         sqlite3_bind_text(cs, 1, ts, -1, SQLITE_TRANSIENT);
         DB_STEP_LOG(cs, "memory_demote");
         sqlite3_reset(cs);
      }
   }

   return total;
}

/* Expire L0 (all) and stale L1 per kind-specific thresholds */
int memory_expire(sqlite3 *db)
{
   int total = 0;

   /* Delete provenance for L0 memories */
   static const char *del_prov_l0 = "DELETE FROM memory_provenance WHERE memory_id IN"
                                    " (SELECT id FROM memories WHERE tier = 'L0')";
   sqlite3_stmt *stmt = db_prepare(db, del_prov_l0);
   if (stmt)
   {
      DB_STEP_LOG(stmt, "memory_expire");
      sqlite3_reset(stmt);
   }

   /* Delete all L0 */
   static const char *del_l0 = "DELETE FROM memories WHERE tier = 'L0'";
   stmt = db_prepare(db, del_l0);
   if (stmt)
   {
      DB_STEP_LOG(stmt, "memory_expire");
      total += sqlite3_changes(db);
      sqlite3_reset(stmt);
   }

   /* Get distinct kinds present at L1 */
   static const char *kinds_sql = "SELECT DISTINCT kind FROM memories WHERE tier = 'L1'";
   sqlite3_stmt *ks = db_prepare(db, kinds_sql);
   if (!ks)
      return total;

   char kinds[16][16];
   int nkinds = 0;
   while (sqlite3_step(ks) == SQLITE_ROW && nkinds < 16)
   {
      const char *k = (const char *)sqlite3_column_text(ks, 0);
      if (k)
         snprintf(kinds[nkinds++], sizeof(kinds[0]), "%s", k);
   }
   sqlite3_reset(ks);

   /* Expire each kind with its own threshold */
   for (int i = 0; i < nkinds; i++)
   {
      kind_lifecycle_t lc;
      kind_lifecycle_load(db, kinds[i], &lc);

      char days_str[16];
      snprintf(days_str, sizeof(days_str), "-%d", lc.expire_days);

      /* Delete provenance for stale L1 of this kind */
      static const char *del_prov = "DELETE FROM memory_provenance WHERE memory_id IN"
                                    " (SELECT id FROM memories WHERE tier = 'L1' AND kind = ?"
                                    "  AND last_used_at < datetime('now', ? || ' days'))";
      sqlite3_stmt *dp = db_prepare(db, del_prov);
      if (dp)
      {
         sqlite3_bind_text(dp, 1, kinds[i], -1, SQLITE_TRANSIENT);
         sqlite3_bind_text(dp, 2, days_str, -1, SQLITE_TRANSIENT);
         DB_STEP_LOG(dp, "memory_expire");
         sqlite3_reset(dp);
      }

      /* Delete stale L1 of this kind */
      static const char *del_l1 = "DELETE FROM memories WHERE tier = 'L1' AND kind = ?"
                                  " AND last_used_at < datetime('now', ? || ' days')";
      sqlite3_stmt *dl = db_prepare(db, del_l1);
      if (dl)
      {
         sqlite3_bind_text(dl, 1, kinds[i], -1, SQLITE_TRANSIENT);
         sqlite3_bind_text(dl, 2, days_str, -1, SQLITE_TRANSIENT);
         DB_STEP_LOG(dl, "memory_expire");
         total += sqlite3_changes(db);
         sqlite3_reset(dl);
      }
   }

   return total;
}

/* Demote memories associated with failed agent executions.
 * Reduces confidence of memories that were used in contexts where
 * the agent failed, creating a negative feedback loop. */
int memory_demote_from_failures(sqlite3 *db)
{
   if (!db)
      return 0;

   /* Find memories whose content keywords appear in recent failed agent prompts.
    * Decay confidence by 0.9 for each association with a failure. */
   static const char *sql =
       "UPDATE memories SET confidence = confidence * 0.9, updated_at = datetime('now')"
       " WHERE tier IN ('L1', 'L2') AND id IN ("
       "   SELECT DISTINCT m.id FROM memories m, agent_log a"
       "   WHERE a.success = 0 AND a.created_at > datetime('now', '-7 days')"
       "   AND (LOWER(a.error) LIKE '%' || LOWER(m.key) || '%')"
       "   AND m.confidence > 0.3"
       " )";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return 0;
   DB_STEP_LOG(stmt, "memory_demote_from_failures");
   int changes = sqlite3_changes(db);
   sqlite3_reset(stmt);
   return changes;
}

/* Synthesize L2 facts from delegation patterns in agent_log.
 * When a role+agent combo has 3+ attempts in 30 days, create a fact
 * summarizing success rate, avg turns, and common errors. */
int memory_promote_delegation_patterns(sqlite3 *db)
{
   if (!db)
      return 0;

   static const char *sql = "SELECT role, agent_name,"
                            " SUM(CASE WHEN success = 1 THEN 1 ELSE 0 END) AS wins,"
                            " SUM(CASE WHEN success = 0 THEN 1 ELSE 0 END) AS fails,"
                            " COUNT(*) AS total,"
                            " CAST(AVG(turns) AS INTEGER) AS avg_turns,"
                            " CAST(AVG(tool_calls) AS INTEGER) AS avg_tools"
                            " FROM agent_log"
                            " WHERE created_at > datetime('now', '-30 days')"
                            " AND role != ''"
                            " GROUP BY role, agent_name"
                            " HAVING total >= 3";

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const char *role = (const char *)sqlite3_column_text(stmt, 0);
      const char *agent = (const char *)sqlite3_column_text(stmt, 1);
      int wins = sqlite3_column_int(stmt, 2);
      int fails = sqlite3_column_int(stmt, 3);
      int total = sqlite3_column_int(stmt, 4);
      int avg_turns = sqlite3_column_int(stmt, 5);
      int avg_tools = sqlite3_column_int(stmt, 6);

      if (!role || !agent)
         continue;

      double success_rate = total > 0 ? (double)wins / total : 0;

      char key[256];
      char content[1024];

      if (success_rate >= 0.7)
      {
         snprintf(key, sizeof(key), "delegate_pattern_%s_%s", role, agent);
         snprintf(content, sizeof(content),
                  "Delegation [%s] via %s reliably succeeds (%d/%d attempts, "
                  "avg %d turns, %d tool calls).",
                  role, agent, wins, total, avg_turns, avg_tools);
         memory_t mem;
         memory_insert(db, TIER_L2, KIND_FACT, key, content, success_rate, "", &mem);
         count++;
      }

      if (fails >= 3 && (double)fails / total >= 0.5)
      {
         /* Find most common error for this role+agent */
         static const char *err_sql = "SELECT error FROM agent_log"
                                      " WHERE role = ? AND agent_name = ? AND success = 0"
                                      " AND error IS NOT NULL AND error != ''"
                                      " AND created_at > datetime('now', '-30 days')"
                                      " ORDER BY id DESC LIMIT 1";
         sqlite3_stmt *estmt = NULL;
         char last_err[256] = "unknown";
         if (sqlite3_prepare_v2(db, err_sql, -1, &estmt, NULL) == SQLITE_OK)
         {
            sqlite3_bind_text(estmt, 1, role, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(estmt, 2, agent, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(estmt) == SQLITE_ROW)
            {
               const char *e = (const char *)sqlite3_column_text(estmt, 0);
               if (e)
                  snprintf(last_err, sizeof(last_err), "%.200s", e);
            }
            sqlite3_finalize(estmt);
         }

         snprintf(key, sizeof(key), "delegate_warning_%s_%s", role, agent);
         snprintf(content, sizeof(content),
                  "Delegation [%s] via %s often fails (%d/%d attempts). "
                  "Recent error: %s",
                  role, agent, fails, total, last_err);
         memory_t mem;
         memory_insert(db, TIER_L2, KIND_FACT, key, content, 0.8, "", &mem);
         count++;
      }
   }
   sqlite3_finalize(stmt);
   return count;
}

/* Synthesize L3 episode memories from recurring delegation failures.
 * When a role+agent combo has FAILURE_EPISODE_MIN+ failures in
 * FAILURE_EPISODE_WINDOW days, create a structured episode. */
int memory_synthesize_failure_episodes(sqlite3 *db)
{
   if (!db)
      return 0;

   char window[8];
   snprintf(window, sizeof(window), "-%d", FAILURE_EPISODE_WINDOW);

   static const char *sql = "SELECT role, agent_name, COUNT(*) AS fails,"
                            " GROUP_CONCAT(error, ' | ') AS errors"
                            " FROM agent_log"
                            " WHERE success = 0"
                            " AND created_at > datetime('now', ?)"
                            " AND error IS NOT NULL AND error != ''"
                            " GROUP BY role, agent_name"
                            " HAVING fails >= ?";

   char window_clause[16];
   snprintf(window_clause, sizeof(window_clause), "-%d days", FAILURE_EPISODE_WINDOW);

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;

   sqlite3_bind_text(stmt, 1, window_clause, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 2, FAILURE_EPISODE_MIN);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const char *role = (const char *)sqlite3_column_text(stmt, 0);
      const char *agent = (const char *)sqlite3_column_text(stmt, 1);
      int fails = sqlite3_column_int(stmt, 2);
      const char *errors = (const char *)sqlite3_column_text(stmt, 3);

      if (!role || !agent)
         continue;

      /* Build episode key with date to avoid duplicates per period */
      char date_buf[16];
      {
         char ts[32];
         now_utc(ts, sizeof(ts));
         /* Extract YYYY-MM-DD */
         snprintf(date_buf, sizeof(date_buf), "%.10s", ts);
      }

      char key[256];
      snprintf(key, sizeof(key), "failure_episode_%s_%s_%s", role, agent, date_buf);

      /* Check if episode already exists for this key */
      {
         static const char *check_sql = "SELECT id FROM memories WHERE key = ? LIMIT 1";
         sqlite3_stmt *cs = db_prepare(db, check_sql);
         if (cs)
         {
            sqlite3_bind_text(cs, 1, key, -1, SQLITE_TRANSIENT);
            int exists = (sqlite3_step(cs) == SQLITE_ROW);
            sqlite3_reset(cs);
            if (exists)
               continue;
         }
      }

      /* Truncate errors to fit in content */
      char err_summary[512];
      if (errors)
         snprintf(err_summary, sizeof(err_summary), "%.500s", errors);
      else
         snprintf(err_summary, sizeof(err_summary), "unknown");

      char content[1024];
      snprintf(content, sizeof(content),
               "Delegation [%s] via %s failed %d times in %d days. "
               "Errors: %s. "
               "Avoid repeating this pattern without addressing the root cause.",
               role, agent, fails, FAILURE_EPISODE_WINDOW, err_summary);

      memory_t mem;
      if (memory_insert(db, TIER_L3, KIND_EPISODE, key, content, 0.7, "", &mem) == 0)
      {
         fprintf(stderr, "L3 episode: %s (%d failures)\n", key, fails);
         count++;
      }
   }
   sqlite3_finalize(stmt);
   return count;
}

/* Embed any L2 memories that lack embeddings (called after promotion). */
static void embed_unembedded_l2(sqlite3 *db)
{
   config_t cfg;
   config_load(&cfg);
   if (!cfg.embedding_command[0])
      return;

   static const char *sql = "SELECT m.id FROM memories m"
                            " LEFT JOIN memory_embeddings e ON e.memory_id = m.id"
                            " WHERE m.tier = 'L2' AND e.memory_id IS NULL"
                            " LIMIT 50";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return;

   int64_t ids[50];
   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < 50)
      ids[count++] = sqlite3_column_int64(stmt, 0);
   sqlite3_reset(stmt);

   for (int i = 0; i < count; i++)
      memory_embed(db, ids[i], cfg.embedding_command);
}

/* --- Context Snapshot and Effectiveness --- */

void memory_record_context_snapshot(sqlite3 *db, const char *session_id, int64_t memory_id,
                                    double relevance_score)
{
   if (!db || !session_id)
      return;
   static const char *sql = "INSERT INTO context_snapshots (session_id, memory_id, relevance_score)"
                            " VALUES (?, ?, ?)";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return;
   sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(stmt, 2, memory_id);
   sqlite3_bind_double(stmt, 3, relevance_score);
   DB_STEP_LOG(stmt, "memory_record_context_snapshot");
   sqlite3_reset(stmt);
}

void memory_record_outcome(sqlite3 *db, const char *session_id, const char *outcome)
{
   if (!db || !session_id || !outcome)
      return;
   static const char *sql = "UPDATE server_sessions SET outcome = ? WHERE id = ?";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return;
   sqlite3_bind_text(stmt, 1, outcome, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, session_id, -1, SQLITE_TRANSIENT);
   DB_STEP_LOG(stmt, "memory_record_outcome");
   sqlite3_reset(stmt);
}

int memory_compute_effectiveness(sqlite3 *db)
{
   if (!db)
      return 0;

   /* Compute effectiveness = (success_present + 1) / (times_surfaced + 2) */
   static const char *sql =
       "UPDATE memories SET effectiveness = ("
       "  SELECT CAST(SUM(CASE WHEN ss.outcome = 'success' THEN 1 ELSE 0 END) + 1 AS REAL)"
       "       / (COUNT(*) + 2)"
       "  FROM context_snapshots cs"
       "  JOIN server_sessions ss ON ss.id = cs.session_id"
       "  WHERE cs.memory_id = memories.id AND ss.outcome IS NOT NULL"
       ")"
       " WHERE id IN ("
       "  SELECT memory_id FROM context_snapshots GROUP BY memory_id HAVING COUNT(*) >= ?);";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return 0;

   sqlite3_bind_int(stmt, 1, EFFECTIVENESS_MIN_SAMPLES);
   DB_STEP_LOG(stmt, "memory_compute_effectiveness");
   int updated = sqlite3_changes(db);
   sqlite3_reset(stmt);
   return updated;
}

int memory_demote_low_effectiveness(sqlite3 *db)
{
   if (!db)
      return 0;

   char ts[32];
   now_utc(ts, sizeof(ts));

   static const char *sql = "UPDATE memories SET tier = 'L1', updated_at = ?"
                            " WHERE tier = 'L2'"
                            " AND effectiveness IS NOT NULL"
                            " AND effectiveness < ?";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return 0;

   sqlite3_bind_text(stmt, 1, ts, -1, SQLITE_TRANSIENT);
   sqlite3_bind_double(stmt, 2, EFFECTIVENESS_DEMOTE_THRESHOLD);
   DB_STEP_LOG(stmt, "memory_demote_low_effectiveness");
   int demoted = sqlite3_changes(db);
   sqlite3_reset(stmt);
   return demoted;
}

int memory_effectiveness_stats(sqlite3 *db, effectiveness_stats_t *out)
{
   if (!db || !out)
      return -1;
   memset(out, 0, sizeof(*out));

   /* Average effectiveness */
   {
      static const char *sql = "SELECT AVG(effectiveness) FROM memories"
                               " WHERE effectiveness IS NOT NULL";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (stmt && sqlite3_step(stmt) == SQLITE_ROW)
         out->avg_effectiveness = sqlite3_column_double(stmt, 0);
      if (stmt)
         sqlite3_reset(stmt);
   }

   /* Low-effectiveness count */
   {
      static const char *sql = "SELECT COUNT(*) FROM memories"
                               " WHERE effectiveness IS NOT NULL AND effectiveness < ?";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (stmt)
      {
         sqlite3_bind_double(stmt, 1, EFFECTIVENESS_DEMOTE_THRESHOLD);
         if (sqlite3_step(stmt) == SQLITE_ROW)
            out->low_effectiveness_count = sqlite3_column_int(stmt, 0);
         sqlite3_reset(stmt);
      }
   }

   /* High-impact count */
   {
      static const char *sql = "SELECT COUNT(*) FROM memories"
                               " WHERE effectiveness IS NOT NULL AND effectiveness > 0.8"
                               " AND use_count >= 10";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (stmt && sqlite3_step(stmt) == SQLITE_ROW)
         out->high_impact_count = sqlite3_column_int(stmt, 0);
      if (stmt)
         sqlite3_reset(stmt);
   }

   /* Never-surfaced L2 */
   {
      static const char *sql = "SELECT COUNT(*) FROM memories m"
                               " WHERE m.tier = 'L2'"
                               " AND NOT EXISTS (SELECT 1 FROM context_snapshots cs"
                               "   WHERE cs.memory_id = m.id)";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (stmt && sqlite3_step(stmt) == SQLITE_ROW)
         out->never_surfaced_l2 = sqlite3_column_int(stmt, 0);
      if (stmt)
         sqlite3_reset(stmt);
   }

   return 0;
}

/* --- Content Safety Retention --- */

int memory_enforce_retention(sqlite3 *db)
{
   if (!db)
      return 0;
   int total = 0;

   /* Restricted: expire after RETENTION_RESTRICTED_DAYS */
   {
      char days[16];
      snprintf(days, sizeof(days), "-%d days", RETENTION_RESTRICTED_DAYS);
      static const char *sql = "DELETE FROM memories"
                               " WHERE sensitivity = 'restricted'"
                               " AND created_at < datetime('now', ?)";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (stmt)
      {
         sqlite3_bind_text(stmt, 1, days, -1, SQLITE_TRANSIENT);
         DB_STEP_LOG(stmt, "memory_enforce_retention");
         total += sqlite3_changes(db);
         sqlite3_reset(stmt);
      }
   }

   /* Sensitive: expire after RETENTION_SENSITIVE_DAYS */
   {
      char days[16];
      snprintf(days, sizeof(days), "-%d days", RETENTION_SENSITIVE_DAYS);
      static const char *sql = "DELETE FROM memories"
                               " WHERE sensitivity = 'sensitive'"
                               " AND created_at < datetime('now', ?)";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (stmt)
      {
         sqlite3_bind_text(stmt, 1, days, -1, SQLITE_TRANSIENT);
         DB_STEP_LOG(stmt, "memory_enforce_retention");
         total += sqlite3_changes(db);
         sqlite3_reset(stmt);
      }
   }

   return total;
}

int memory_run_maintenance(sqlite3 *db, int *promoted, int *demoted, int *expired)
{
   int p = memory_promote(db);
   p += memory_promote_delegation_patterns(db);
   p += memory_synthesize_failure_episodes(db);
   int d = memory_demote(db);
   d += memory_demote_from_failures(db);
   d += memory_demote_low_effectiveness(db);
   int e = memory_expire(db);
   e += memory_enforce_retention(db);

   if (promoted)
      *promoted = p;
   if (demoted)
      *demoted = d;
   if (expired)
      *expired = e;

   /* Embed newly promoted L2 memories that lack embeddings */
   if (p > 0)
      embed_unembedded_l2(db);

   /* Compute memory effectiveness scores */
   memory_compute_effectiveness(db);

   /* Retroactive conflict detection (daily, rate-limited) */
   memory_scan_retroactive_conflicts(db);

   /* Record health metrics for this maintenance cycle */
   memory_record_health(db, p, d, e);

   /* Prune old health and contradiction_log rows (90-day retention) */
   memory_prune_health(db);

   /* Prune graph edges where both endpoints have no L1+ memory */
   memory_graph_prune(db);

   return 0;
}

/* --- Health Metrics --- */

void memory_record_health(sqlite3 *db, int promotions, int demotions, int expirations)
{
   if (!db)
      return;

   char ts[32];
   now_utc(ts, sizeof(ts));

   /* Count total memories */
   int total = 0;
   {
      static const char *sql = "SELECT COUNT(*) FROM memories";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (stmt)
      {
         if (sqlite3_step(stmt) == SQLITE_ROW)
            total = sqlite3_column_int(stmt, 0);
         sqlite3_reset(stmt);
      }
   }

   /* Count contradictions detected since last cycle (unresolved in memory_conflicts) */
   int contradictions = 0;
   {
      static const char *sql = "SELECT COUNT(*) FROM memory_conflicts"
                               " WHERE detected_at > datetime('now', '-1 day')";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (stmt)
      {
         if (sqlite3_step(stmt) == SQLITE_ROW)
            contradictions = sqlite3_column_int(stmt, 0);
         sqlite3_reset(stmt);
      }
   }

   static const char *sql = "INSERT INTO memory_health"
                            " (cycle_at, total_memories, contradictions_detected,"
                            "  promotions, demotions, expirations)"
                            " VALUES (?, ?, ?, ?, ?, ?)";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return;

   sqlite3_bind_text(stmt, 1, ts, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 2, total);
   sqlite3_bind_int(stmt, 3, contradictions);
   sqlite3_bind_int(stmt, 4, promotions);
   sqlite3_bind_int(stmt, 5, demotions);
   sqlite3_bind_int(stmt, 6, expirations);
   DB_STEP_LOG(stmt, "memory_record_health");
   sqlite3_reset(stmt);
}

void memory_prune_health(sqlite3 *db)
{
   if (!db)
      return;

   static const char *sql1 = "DELETE FROM memory_health"
                             " WHERE cycle_at < datetime('now', '-90 days')";
   sqlite3_stmt *stmt = db_prepare(db, sql1);
   if (stmt)
   {
      DB_STEP_LOG(stmt, "memory_prune_health");
      sqlite3_reset(stmt);
   }

   static const char *sql2 = "DELETE FROM contradiction_log"
                             " WHERE detected_at < datetime('now', '-90 days')";
   stmt = db_prepare(db, sql2);
   if (stmt)
   {
      DB_STEP_LOG(stmt, "memory_prune_health");
      sqlite3_reset(stmt);
   }
}

int memory_query_health(sqlite3 *db, memory_health_t *out)
{
   memset(out, 0, sizeof(*out));
   if (!db)
      return -1;

   /* Aggregate from memory_health over last 7 days */
   static const char *sql = "SELECT COUNT(*), SUM(contradictions_detected), SUM(promotions),"
                            " SUM(demotions), SUM(expirations)"
                            " FROM memory_health"
                            " WHERE cycle_at > datetime('now', '-7 days')";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (stmt)
   {
      if (sqlite3_step(stmt) == SQLITE_ROW)
      {
         out->cycles = sqlite3_column_int(stmt, 0);
         out->total_contradictions = sqlite3_column_int(stmt, 1);
         out->total_promotions = sqlite3_column_int(stmt, 2);
         out->total_demotions = sqlite3_column_int(stmt, 3);
         out->total_expirations = sqlite3_column_int(stmt, 4);
      }
      sqlite3_reset(stmt);
   }

   /* Contradiction rate: contradictions / new memories inserted in 7 days */
   {
      static const char *sql2 = "SELECT COUNT(*) FROM memories"
                                " WHERE created_at > datetime('now', '-7 days')";
      sqlite3_stmt *s = db_prepare(db, sql2);
      if (s)
      {
         if (sqlite3_step(s) == SQLITE_ROW)
         {
            int new_mems = sqlite3_column_int(s, 0);
            if (new_mems > 0)
               out->contradiction_rate = (double)out->total_contradictions / new_mems;
         }
         sqlite3_reset(s);
      }
   }

   /* Promotion rate: promotions / L1 eligible (use_count >= threshold OR confidence >= threshold)
    */
   {
      static const char *sql3 = "SELECT COUNT(*) FROM memories"
                                " WHERE tier = 'L1'"
                                " AND (use_count >= ? OR confidence >= ?)";
      sqlite3_stmt *s = db_prepare(db, sql3);
      if (s)
      {
         sqlite3_bind_int(s, 1, PROMOTE_L1_USE_COUNT);
         sqlite3_bind_double(s, 2, PROMOTE_L1_CONFIDENCE);
         if (sqlite3_step(s) == SQLITE_ROW)
         {
            int eligible = sqlite3_column_int(s, 0);
            /* eligible now is post-promotion; estimate pre-promotion count */
            int pre_eligible = eligible + out->total_promotions;
            if (pre_eligible > 0)
               out->promotion_rate = (double)out->total_promotions / pre_eligible;
         }
         sqlite3_reset(s);
      }
   }

   /* Demotion rate: demotions / total L2 */
   {
      static const char *sql4 = "SELECT COUNT(*) FROM memories WHERE tier = 'L2'";
      sqlite3_stmt *s = db_prepare(db, sql4);
      if (s)
      {
         if (sqlite3_step(s) == SQLITE_ROW)
         {
            int l2_total = sqlite3_column_int(s, 0);
            int pre_total = l2_total + out->total_demotions;
            if (pre_total > 0)
               out->demotion_rate = (double)out->total_demotions / pre_total;
         }
         sqlite3_reset(s);
      }
   }

   /* Staleness: % of L2 facts with last_used_at NULL or > 30 days ago */
   {
      static const char *sql5 =
          "SELECT COUNT(*) FROM memories WHERE tier = 'L2'"
          " AND (last_used_at IS NULL OR last_used_at < datetime('now', '-30 days'))";
      sqlite3_stmt *s = db_prepare(db, sql5);
      if (s)
      {
         if (sqlite3_step(s) == SQLITE_ROW)
         {
            int stale = sqlite3_column_int(s, 0);
            static const char *sql6 = "SELECT COUNT(*) FROM memories WHERE tier = 'L2'";
            sqlite3_stmt *s2 = db_prepare(db, sql6);
            if (s2)
            {
               if (sqlite3_step(s2) == SQLITE_ROW)
               {
                  int l2_total = sqlite3_column_int(s2, 0);
                  if (l2_total > 0)
                     out->staleness = (double)stale / l2_total;
               }
               sqlite3_reset(s2);
            }
         }
         sqlite3_reset(s);
      }
   }

   return 0;
}

/* --- Session Folding --- */

int memory_fold_session(sqlite3 *db, const char *session_id)
{
   if (!db || !session_id)
      return -1;

   /* Collect L0 keys and content for this session */
   static const char *sql = "SELECT key, content FROM memories"
                            " WHERE tier = 'L0' AND source_session = ?"
                            " ORDER BY created_at ASC";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return -1;

   sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);

   char checkpoint[200];
   int pos = 0;
   int count = 0;

   while (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const char *k = (const char *)sqlite3_column_text(stmt, 0);
      const char *c = (const char *)sqlite3_column_text(stmt, 1);
      if (!k)
         continue;

      const char *text = (c && c[0]) ? c : k;
      int remaining = (int)sizeof(checkpoint) - pos - 3;
      if (remaining <= 0)
         break;

      if (pos > 0)
      {
         checkpoint[pos++] = ';';
         checkpoint[pos++] = ' ';
         remaining -= 2;
      }

      int len = (int)strlen(text);
      if (len > remaining)
         len = remaining;
      memcpy(checkpoint + pos, text, len);
      pos += len;
      count++;
   }
   sqlite3_reset(stmt);

   if (count == 0)
      return 0;

   checkpoint[pos] = '\0';

   /* INSERT as L1 episode */
   char ep_key[256];
   snprintf(ep_key, sizeof(ep_key), "session:%s", session_id);

   memory_insert(db, TIER_L1, KIND_EPISODE, ep_key, checkpoint, 0.8, session_id, NULL);

   /* Delete L0 provenance for this session */
   static const char *del_prov = "DELETE FROM memory_provenance WHERE memory_id IN"
                                 " (SELECT id FROM memories WHERE tier = 'L0'"
                                 "  AND source_session = ?)";
   stmt = db_prepare(db, del_prov);
   if (stmt)
   {
      sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);
      DB_STEP_LOG(stmt, "memory_fold_session");
      sqlite3_reset(stmt);
   }

   /* Delete L0 memories for this session */
   static const char *del_mem = "DELETE FROM memories WHERE tier = 'L0'"
                                " AND source_session = ?";
   stmt = db_prepare(db, del_mem);
   if (stmt)
   {
      sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);
      DB_STEP_LOG(stmt, "memory_fold_session");
      sqlite3_reset(stmt);
   }

   return 0;
}

/* --- Conflict Detection --- */

int64_t memory_detect_conflict(sqlite3 *db, const char *key, const char *content)
{
   if (!db || !key)
      return 0;

   static const char *sql = "SELECT id, content FROM memories WHERE key = ?";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return 0;

   sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);

   int64_t conflict_id = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const char *existing = (const char *)sqlite3_column_text(stmt, 1);
      if (existing && content && is_contradiction(existing, content))
      {
         conflict_id = sqlite3_column_int64(stmt, 0);
         break;
      }
   }
   sqlite3_reset(stmt);
   return conflict_id;
}

int memory_record_conflict(sqlite3 *db, int64_t mem_a, int64_t mem_b)
{
   char ts[32];
   now_utc(ts, sizeof(ts));

   static const char *sql = "INSERT INTO memory_conflicts (memory_a, memory_b, detected_at,"
                            " resolved) VALUES (?, ?, ?, 0)";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return -1;

   sqlite3_bind_int64(stmt, 1, mem_a);
   sqlite3_bind_int64(stmt, 2, mem_b);
   sqlite3_bind_text(stmt, 3, ts, -1, SQLITE_TRANSIENT);

   if (sqlite3_step(stmt) != SQLITE_DONE)
   {
      sqlite3_reset(stmt);
      return -1;
   }
   sqlite3_reset(stmt);

   /* Also write to contradiction_log audit trail */
   memory_log_contradiction(db, mem_a, mem_b, "pending", NULL);

   /* Auto-link: contradicts relationship */
   memory_link_create(db, mem_a, mem_b, "contradicts");

   return 0;
}

void memory_log_contradiction(sqlite3 *db, int64_t mem_a, int64_t mem_b, const char *resolution,
                              const char *details)
{
   if (!db)
      return;

   char ts[32];
   now_utc(ts, sizeof(ts));

   static const char *sql = "INSERT INTO contradiction_log"
                            " (detected_at, memory_a_id, memory_b_id, resolution, details)"
                            " VALUES (?, ?, ?, ?, ?)";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return;

   sqlite3_bind_text(stmt, 1, ts, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(stmt, 2, mem_a);
   sqlite3_bind_int64(stmt, 3, mem_b);
   sqlite3_bind_text(stmt, 4, resolution ? resolution : "pending", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 5, details, -1, SQLITE_TRANSIENT);
   DB_STEP_LOG(stmt, "memory_log_contradiction");
   sqlite3_reset(stmt);

   fprintf(stderr, "contradiction: memory %lld vs %lld (%s)\n", (long long)mem_a, (long long)mem_b,
           resolution ? resolution : "pending");
}

/* --- Retroactive Conflict Detection --- */

/* Check if enough time has passed since the last retroactive scan */
static int retro_scan_due(sqlite3 *db)
{
   /* Check contradiction_log for most recent retroactive scan marker */
   static const char *sql2 = "SELECT MAX(detected_at) FROM contradiction_log"
                             " WHERE details = 'retroactive_scan'";
   sqlite3_stmt *stmt = db_prepare(db, sql2);
   if (!stmt)
      return 1; /* No table or error — allow scan */

   int due = 1;
   if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL)
   {
      const char *last = (const char *)sqlite3_column_text(stmt, 0);
      if (last)
      {
         /* Check if last scan was more than RETRO_CONFLICT_INTERVAL seconds ago */
         static const char *age_sql = "SELECT strftime('%s', 'now') - strftime('%s', ?)";
         sqlite3_stmt *age_stmt = db_prepare(db, age_sql);
         if (age_stmt)
         {
            sqlite3_bind_text(age_stmt, 1, last, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(age_stmt) == SQLITE_ROW)
            {
               int elapsed = sqlite3_column_int(age_stmt, 0);
               if (elapsed < RETRO_CONFLICT_INTERVAL)
                  due = 0;
            }
            sqlite3_reset(age_stmt);
         }
      }
   }
   sqlite3_reset(stmt);
   return due;
}

int memory_scan_retroactive_conflicts(sqlite3 *db)
{
   if (!db)
      return 0;

   /* Rate limit: at most once per day */
   if (!retro_scan_due(db))
      return 0;

   /* Skip if fewer than RETRO_CONFLICT_MIN_L2 L2 memories */
   {
      static const char *sql = "SELECT COUNT(*) FROM memories WHERE tier = 'L2'";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (stmt)
      {
         if (sqlite3_step(stmt) == SQLITE_ROW)
         {
            int count = sqlite3_column_int(stmt, 0);
            sqlite3_reset(stmt);
            if (count < RETRO_CONFLICT_MIN_L2)
               return 0;
         }
         else
         {
            sqlite3_reset(stmt);
            return 0;
         }
      }
   }

   int conflicts_found = 0;

   /* 1. Cross-key scan: L2 memories with overlapping terms but different keys */
   {
      static const char *sql = "SELECT a.id, b.id, a.content, b.content"
                               " FROM memories a, memories b"
                               " WHERE a.tier = 'L2' AND b.tier = 'L2'"
                               " AND a.id < b.id"
                               " AND a.key != b.key"
                               " AND a.confidence > 0.5 AND b.confidence > 0.5"
                               " AND ("
                               "   LOWER(a.key || ' ' || a.content) LIKE"
                               "     '%' || SUBSTR(b.key, 1, INSTR(b.key || '_', '_') - 1) || '%'"
                               "   OR LOWER(b.key || ' ' || b.content) LIKE"
                               "     '%' || SUBSTR(a.key, 1, INSTR(a.key || '_', '_') - 1) || '%'"
                               " )"
                               " LIMIT ?";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (stmt)
      {
         sqlite3_bind_int(stmt, 1, RETRO_CONFLICT_MAX_PAIRS);
         while (sqlite3_step(stmt) == SQLITE_ROW)
         {
            int64_t id_a = sqlite3_column_int64(stmt, 0);
            int64_t id_b = sqlite3_column_int64(stmt, 1);
            const char *content_a = (const char *)sqlite3_column_text(stmt, 2);
            const char *content_b = (const char *)sqlite3_column_text(stmt, 3);

            if (content_a && content_b && is_contradiction(content_a, content_b))
            {
               memory_record_conflict(db, id_a, id_b);
               conflicts_found++;
            }
         }
         sqlite3_reset(stmt);
      }
   }

   /* 2. Cross-kind scan: facts vs decisions */
   {
      static const char *sql = "SELECT f.id, d.id, f.content, d.content"
                               " FROM memories f, memories d"
                               " WHERE f.kind = 'fact' AND d.kind = 'decision'"
                               " AND f.tier IN ('L1', 'L2') AND d.tier IN ('L2', 'L3')"
                               " AND f.id != d.id"
                               " AND f.confidence > 0.5 AND d.confidence > 0.5"
                               " AND (LOWER(f.content) LIKE '%' || LOWER(d.key) || '%'"
                               "      OR LOWER(d.content) LIKE '%' || LOWER(f.key) || '%')"
                               " LIMIT ?";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (stmt)
      {
         sqlite3_bind_int(stmt, 1, RETRO_CONFLICT_MAX_PAIRS);
         while (sqlite3_step(stmt) == SQLITE_ROW)
         {
            int64_t id_f = sqlite3_column_int64(stmt, 0);
            int64_t id_d = sqlite3_column_int64(stmt, 1);
            const char *content_f = (const char *)sqlite3_column_text(stmt, 2);
            const char *content_d = (const char *)sqlite3_column_text(stmt, 3);

            if (content_f && content_d && is_contradiction(content_f, content_d))
            {
               memory_record_conflict(db, id_f, id_d);
               conflicts_found++;
            }
         }
         sqlite3_reset(stmt);
      }
   }

   /* Record scan marker in contradiction_log for rate limiting */
   {
      char ts[32];
      now_utc(ts, sizeof(ts));
      static const char *sql = "INSERT INTO contradiction_log"
                               " (detected_at, memory_a_id, memory_b_id, resolution, details)"
                               " VALUES (?, 0, 0, 'scan', 'retroactive_scan')";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (stmt)
      {
         sqlite3_bind_text(stmt, 1, ts, -1, SQLITE_TRANSIENT);
         DB_STEP_LOG(stmt, "memory_promote");
         sqlite3_reset(stmt);
      }
   }

   if (conflicts_found > 0)
      fprintf(stderr, "retroactive scan: %d conflict(s) detected\n", conflicts_found);

   return conflicts_found;
}

int memory_list_conflicts(sqlite3 *db, conflict_t *out, int max)
{
   static const char *sql = "SELECT id, memory_a, memory_b, detected_at, resolved, resolution"
                            " FROM memory_conflicts WHERE resolved = 0"
                            " ORDER BY detected_at DESC";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return 0;

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max)
   {
      out[count].id = sqlite3_column_int64(stmt, 0);
      out[count].memory_a = sqlite3_column_int64(stmt, 1);
      out[count].memory_b = sqlite3_column_int64(stmt, 2);

      const char *dat = (const char *)sqlite3_column_text(stmt, 3);
      snprintf(out[count].detected_at, sizeof(out[count].detected_at), "%s", dat ? dat : "");

      out[count].resolved = sqlite3_column_int(stmt, 4);

      const char *res = (const char *)sqlite3_column_text(stmt, 5);
      snprintf(out[count].resolution, sizeof(out[count].resolution), "%s", res ? res : "");
      count++;
   }
   sqlite3_reset(stmt);
   return count;
}

int memory_resolve_conflict(sqlite3 *db, int64_t conflict_id, const char *resolution)
{
   /* Fetch the conflict's memory IDs before resolving */
   int64_t mem_a = 0, mem_b = 0;
   {
      static const char *sel = "SELECT memory_a, memory_b FROM memory_conflicts WHERE id = ?";
      sqlite3_stmt *ss = db_prepare(db, sel);
      if (ss)
      {
         sqlite3_bind_int64(ss, 1, conflict_id);
         if (sqlite3_step(ss) == SQLITE_ROW)
         {
            mem_a = sqlite3_column_int64(ss, 0);
            mem_b = sqlite3_column_int64(ss, 1);
         }
         sqlite3_reset(ss);
      }
   }

   static const char *sql = "UPDATE memory_conflicts SET resolved = 1, resolution = ?"
                            " WHERE id = ?";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return -1;

   sqlite3_bind_text(stmt, 1, resolution ? resolution : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(stmt, 2, conflict_id);
   DB_STEP_LOG(stmt, "memory_resolve_conflict");
   int changes = sqlite3_changes(db);
   sqlite3_reset(stmt);

   /* Log the resolution to the audit trail */
   if (changes > 0 && mem_a > 0)
      memory_log_contradiction(db, mem_a, mem_b, resolution ? resolution : "resolved", NULL);

   return changes > 0 ? 0 : -1;
}
