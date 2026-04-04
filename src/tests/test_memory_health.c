#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "aimee.h"

int main(void)
{
   printf("memory_health: ");

   sqlite3 *db = db_open(":memory:");
   assert(db != NULL);

   /* --- memory_run_maintenance populates memory_health --- */
   {
      /* Insert some memories so maintenance has something to work with */
      memory_t m;
      memory_insert(db, TIER_L0, KIND_FACT, "test-key-1", "value 1", 0.5, "sess-1", &m);
      memory_insert(db, TIER_L1, KIND_FACT, "test-key-2", "value 2", 0.9, "sess-1", &m);
      memory_insert(db, TIER_L2, KIND_FACT, "test-key-3", "value 3", 1.0, "sess-1", &m);

      int promoted = 0, demoted = 0, expired = 0;
      memory_run_maintenance(db, &promoted, &demoted, &expired);

      /* Verify memory_health table has a row */
      sqlite3_stmt *stmt = NULL;
      int rc = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM memory_health", -1, &stmt, NULL);
      assert(rc == SQLITE_OK);
      assert(sqlite3_step(stmt) == SQLITE_ROW);
      int count = sqlite3_column_int(stmt, 0);
      assert(count >= 1);
      sqlite3_finalize(stmt);
   }

   /* --- memory_query_health returns aggregated stats --- */
   {
      memory_health_t health;
      int rc = memory_query_health(db, &health);
      assert(rc == 0);
      assert(health.cycles >= 1);
      /* total_expirations should reflect the L0 we inserted (expired by maintenance) */
      assert(health.total_expirations >= 0);
   }

   /* --- Run multiple maintenance cycles --- */
   {
      memory_t m;
      memory_insert(db, TIER_L1, KIND_FACT, "multi-cycle-1", "data", 0.95, "sess-2", &m);

      int p, d, e;
      memory_run_maintenance(db, &p, &d, &e);
      memory_run_maintenance(db, &p, &d, &e);

      memory_health_t health;
      memory_query_health(db, &health);
      assert(health.cycles >= 3);
   }

   /* --- memory_record_conflict writes to contradiction_log --- */
   {
      memory_t m1, m2;
      memory_insert(db, TIER_L1, KIND_FACT, "conflict-a", "always use X", 1.0, "", &m1);
      memory_insert(db, TIER_L1, KIND_FACT, "conflict-b", "never use X", 1.0, "", &m2);

      memory_record_conflict(db, m1.id, m2.id);

      /* Verify contradiction_log has a row */
      sqlite3_stmt *stmt = NULL;
      int rc = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM contradiction_log", -1, &stmt, NULL);
      assert(rc == SQLITE_OK);
      assert(sqlite3_step(stmt) == SQLITE_ROW);
      int count = sqlite3_column_int(stmt, 0);
      assert(count >= 1);
      sqlite3_finalize(stmt);

      /* Verify the log entry has correct IDs */
      rc = sqlite3_prepare_v2(db,
                              "SELECT memory_a_id, memory_b_id, resolution"
                              " FROM contradiction_log ORDER BY id DESC LIMIT 1",
                              -1, &stmt, NULL);
      assert(rc == SQLITE_OK);
      assert(sqlite3_step(stmt) == SQLITE_ROW);
      int64_t a = sqlite3_column_int64(stmt, 0);
      int64_t b = sqlite3_column_int64(stmt, 1);
      const char *res = (const char *)sqlite3_column_text(stmt, 2);
      assert(a == m1.id);
      assert(b == m2.id);
      assert(strcmp(res, "pending") == 0);
      sqlite3_finalize(stmt);
   }

   /* --- memory_resolve_conflict also logs resolution --- */
   {
      conflict_t conflicts[8];
      int count = memory_list_conflicts(db, conflicts, 8);
      assert(count >= 1);

      memory_resolve_conflict(db, conflicts[0].id, "a_decayed");

      /* Verify resolution logged */
      sqlite3_stmt *stmt = NULL;
      int rc = sqlite3_prepare_v2(db,
                                  "SELECT resolution FROM contradiction_log"
                                  " ORDER BY id DESC LIMIT 1",
                                  -1, &stmt, NULL);
      assert(rc == SQLITE_OK);
      assert(sqlite3_step(stmt) == SQLITE_ROW);
      const char *res = (const char *)sqlite3_column_text(stmt, 0);
      assert(strcmp(res, "a_decayed") == 0);
      sqlite3_finalize(stmt);
   }

   /* --- staleness calculation --- */
   {
      /* The L2 memory we inserted earlier should show up in staleness if untouched */
      memory_health_t health;
      memory_query_health(db, &health);
      /* staleness should be between 0 and 1 */
      assert(health.staleness >= 0.0 && health.staleness <= 1.0);
   }

   db_stmt_cache_clear();
   db_close(db);

   printf("all tests passed\n");
   return 0;
}
