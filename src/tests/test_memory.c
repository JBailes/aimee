#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "aimee.h"

static sqlite3 *setup(void)
{
   sqlite3 *db = db_open(":memory:");
   assert(db != NULL);
   return db;
}

static void teardown(sqlite3 *db)
{
   db_stmt_cache_clear();
   db_close(db);
}

static void test_insert_memory(void)
{
   sqlite3 *db = setup();
   memory_t m;
   int rc = memory_insert(db, TIER_L1, KIND_FACT, "test key", "test content", 0.8, "s1", &m);
   assert(rc == 0);
   assert(strcmp(m.tier, TIER_L1) == 0);
   assert(strcmp(m.kind, KIND_FACT) == 0);
   assert(m.confidence >= 0.79 && m.confidence <= 0.81);
   assert(m.use_count >= 1);
   teardown(db);
}

static void test_insert_merge(void)
{
   sqlite3 *db = setup();
   memory_t m1, m2;
   memory_insert(db, TIER_L1, KIND_FACT, "dup key", "old content", 0.5, "s1", &m1);
   memory_insert(db, TIER_L1, KIND_FACT, "dup key", "new content", 0.9, "s2", &m2);

   assert(m2.id == m1.id); /* merged, same ID */
   assert(m2.use_count >= 2);
   assert(m2.confidence >= 0.89); /* kept higher */
   teardown(db);
}

static void test_touch_memory(void)
{
   sqlite3 *db = setup();
   memory_t m;
   memory_insert(db, TIER_L2, KIND_PREFERENCE, "style", "concise", 0.8, "s1", &m);
   memory_touch(db, m.id);

   memory_t updated;
   memory_get(db, m.id, &updated);
   assert(updated.use_count >= 2);
   teardown(db);
}

static void test_promote(void)
{
   sqlite3 *db = setup();
   memory_t m;
   memory_insert(db, TIER_L1, KIND_FACT, "promote me", "important", 0.5, "s1", &m);

   /* Bump use_count above threshold */
   for (int i = 0; i < PROMOTE_L1_USE_COUNT; i++)
      memory_touch(db, m.id);

   int promoted = memory_promote(db);
   assert(promoted >= 1);

   memory_t updated;
   memory_get(db, m.id, &updated);
   assert(strcmp(updated.tier, TIER_L2) == 0);
   teardown(db);
}

static void test_expire_l0(void)
{
   sqlite3 *db = setup();
   memory_t m;
   memory_insert(db, TIER_L0, KIND_SCRATCH, "temp", "scratch data", 0.3, "s1", &m);

   int expired = memory_expire(db);
   assert(expired >= 1);

   memory_t check;
   int rc = memory_get(db, m.id, &check);
   assert(rc != 0); /* should be gone */
   teardown(db);
}

static void test_fold_session(void)
{
   sqlite3 *db = setup();
   memory_t m;
   memory_insert(db, TIER_L0, KIND_SCRATCH, "task1", "content 1", 0.5, "fold-sess", &m);
   memory_insert(db, TIER_L0, KIND_SCRATCH, "task2", "content 2", 0.5, "fold-sess", &m);

   int rc = memory_fold_session(db, "fold-sess");
   assert(rc == 0);

   /* L0 should be gone */
   memory_t l0[10];
   int n = memory_list(db, TIER_L0, "", 10, l0, 10);
   assert(n == 0);

   /* Should have L1 checkpoint */
   memory_t l1[10];
   n = memory_list(db, TIER_L1, KIND_EPISODE, 10, l1, 10);
   assert(n >= 1);
   teardown(db);
}

static void test_stats(void)
{
   sqlite3 *db = setup();
   memory_t m;
   memory_insert(db, TIER_L2, KIND_FACT, "f1", "fact one", 0.9, "s1", &m);
   memory_insert(db, TIER_L1, KIND_PREFERENCE, "p1", "pref", 0.7, "s1", &m);

   memory_stats_t s;
   memory_stats(db, &s);
   assert(s.total == 2);
   teardown(db);
}

static void test_delete_memory(void)
{
   sqlite3 *db = setup();
   memory_t m;
   memory_insert(db, TIER_L1, KIND_FACT, "deleteme", "temp", 0.5, "s1", &m);
   int rc = memory_delete(db, m.id);
   assert(rc == 0);

   memory_t check;
   rc = memory_get(db, m.id, &check);
   assert(rc != 0);
   teardown(db);
}

/* --- Deeper coverage --- */

static void test_list_by_tier_and_kind(void)
{
   sqlite3 *db = setup();
   memory_t out;
   memory_insert(db, TIER_L0, KIND_FACT, "l0-fact", "content", 1.0, "s1", &out);
   memory_insert(db, TIER_L1, KIND_FACT, "l1-fact", "content", 1.0, "s1", &out);
   memory_insert(db, TIER_L1, KIND_PREFERENCE, "l1-pref", "content", 1.0, "s1", &out);
   memory_insert(db, TIER_L2, KIND_DECISION, "l2-dec", "content", 1.0, "s1", &out);

   memory_t results[16];

   /* Filter by tier */
   int count = memory_list(db, TIER_L1, NULL, 10, results, 16);
   assert(count == 2);

   /* Filter by kind */
   count = memory_list(db, NULL, KIND_FACT, 10, results, 16);
   assert(count == 2);

   /* Filter by both */
   count = memory_list(db, TIER_L1, KIND_FACT, 10, results, 16);
   assert(count == 1);
   assert(strcmp(results[0].key, "l1-fact") == 0);

   /* No matches */
   count = memory_list(db, TIER_L3, KIND_EPISODE, 10, results, 16);
   assert(count == 0);

   teardown(db);
}

static void test_get_nonexistent(void)
{
   sqlite3 *db = setup();
   memory_t out;
   int rc = memory_get(db, 99999, &out);
   assert(rc != 0);
   teardown(db);
}

static void test_delete_nonexistent(void)
{
   sqlite3 *db = setup();
   int rc = memory_delete(db, 99999);
   /* Should not crash, may return 0 or error */
   (void)rc;
   teardown(db);
}

static void test_insert_empty_content(void)
{
   sqlite3 *db = setup();
   memory_t out;
   int rc = memory_insert(db, TIER_L0, KIND_FACT, "empty", "", 1.0, "s1", &out);
   assert(rc == 0);
   assert(out.id > 0);

   memory_t loaded;
   rc = memory_get(db, out.id, &loaded);
   assert(rc == 0);
   assert(loaded.content[0] == '\0');
   teardown(db);
}

static void test_confidence_bounds(void)
{
   sqlite3 *db = setup();
   memory_t out;

   /* Zero confidence */
   int rc = memory_insert(db, TIER_L0, KIND_FACT, "low", "content", 0.0, "s1", &out);
   assert(rc == 0);
   assert(out.confidence < 0.01);

   /* High confidence */
   rc = memory_insert(db, TIER_L0, KIND_FACT, "high", "content", 1.0, "s1", &out);
   assert(rc == 0);
   assert(out.confidence > 0.99);

   teardown(db);
}

static void test_list_respects_limit(void)
{
   sqlite3 *db = setup();
   memory_t out;
   for (int i = 0; i < 10; i++)
   {
      char key[32];
      snprintf(key, sizeof(key), "limit-test-%d", i);
      memory_insert(db, TIER_L0, KIND_FACT, key, "content", 1.0, "s1", &out);
   }

   memory_t results[16];
   int count = memory_list(db, TIER_L0, NULL, 3, results, 16);
   assert(count == 3);

   count = memory_list(db, TIER_L0, NULL, 100, results, 16);
   assert(count == 10);

   teardown(db);
}

static void test_run_maintenance_cycle(void)
{
   sqlite3 *db = setup();
   memory_t out;

   /* Create some L0 memories with high use_count (should promote) */
   memory_insert(db, TIER_L0, KIND_FACT, "promote-me", "content", 0.95, "s1", &out);
   for (int i = 0; i < PROMOTE_L1_USE_COUNT + 1; i++)
      memory_touch(db, out.id);

   int promoted = 0, demoted = 0, expired = 0;
   memory_run_maintenance(db, &promoted, &demoted, &expired);
   assert(promoted >= 0);
   assert(demoted >= 0);
   assert(expired >= 0);

   teardown(db);
}

int main(void)
{
   test_insert_memory();
   test_insert_merge();
   test_touch_memory();
   test_promote();
   test_expire_l0();
   test_fold_session();
   test_stats();
   test_delete_memory();
   test_list_by_tier_and_kind();
   test_get_nonexistent();
   test_delete_nonexistent();
   test_insert_empty_content();
   test_confidence_bounds();
   test_list_respects_limit();
   test_run_maintenance_cycle();

   /* --- cosine_similarity: known vectors --- */
   {
      float a[] = {1.0f, 0.0f, 0.0f};
      float b[] = {1.0f, 0.0f, 0.0f};
      double sim = cosine_similarity(a, b, 3);
      assert(fabs(sim - 1.0) < 0.001); /* identical vectors = 1.0 */

      float c[] = {0.0f, 1.0f, 0.0f};
      sim = cosine_similarity(a, c, 3);
      assert(fabs(sim) < 0.001); /* orthogonal vectors = 0.0 */

      float d[] = {-1.0f, 0.0f, 0.0f};
      sim = cosine_similarity(a, d, 3);
      assert(fabs(sim + 1.0) < 0.001); /* opposite vectors = -1.0 */

      float e[] = {1.0f, 1.0f, 0.0f};
      sim = cosine_similarity(a, e, 3);
      assert(fabs(sim - 0.7071) < 0.01); /* 45-degree angle */
   }

   /* --- embedding storage and retrieval --- */
   {
      sqlite3 *db = setup();
      memory_t mem;
      memory_insert(db, TIER_L2, KIND_FACT, "embed-test", "test content", 0.9, "", &mem);

      /* Manually store an embedding blob */
      float vec[] = {0.1f, 0.2f, 0.3f, 0.4f};
      char ts[32];
      now_utc(ts, sizeof(ts));

      static const char *sql = "INSERT INTO memory_embeddings"
                               " (memory_id, embedding, model, created_at)"
                               " VALUES (?, ?, ?, ?)";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      assert(stmt != NULL);
      sqlite3_bind_int64(stmt, 1, mem.id);
      sqlite3_bind_blob(stmt, 2, vec, sizeof(vec), SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 3, "test-model", -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 4, ts, -1, SQLITE_TRANSIENT);
      assert(sqlite3_step(stmt) == SQLITE_DONE);
      sqlite3_reset(stmt);

      /* Retrieve and verify */
      static const char *sel = "SELECT embedding FROM memory_embeddings WHERE memory_id = ?";
      sqlite3_stmt *ss = db_prepare(db, sel);
      assert(ss != NULL);
      sqlite3_bind_int64(ss, 1, mem.id);
      assert(sqlite3_step(ss) == SQLITE_ROW);
      const float *got = (const float *)sqlite3_column_blob(ss, 0);
      int bytes = sqlite3_column_bytes(ss, 0);
      assert(bytes == (int)sizeof(vec));
      assert(fabs(got[0] - 0.1f) < 0.001f);
      assert(fabs(got[3] - 0.4f) < 0.001f);
      sqlite3_reset(ss);

      /* Delete memory — embedding should cascade */
      memory_delete(db, mem.id);
      sqlite3_bind_int64(ss, 1, mem.id);
      assert(sqlite3_step(ss) != SQLITE_ROW);
      sqlite3_reset(ss);

      db_stmt_cache_clear();
      db_close(db);
   }

   /* --- graceful fallback: no embedding command configured --- */
   {
      /* memory_embed_text with empty command returns 0 */
      float vec[4];
      int dim = memory_embed_text("test", "", vec, 4);
      assert(dim == 0);

      dim = memory_embed_text("test", NULL, vec, 4);
      assert(dim == 0);
   }

   /* --- kind_lifecycle_load: returns correct defaults for all 8 kinds --- */
   {
      sqlite3 *db = setup();
      kind_lifecycle_t lc;

      /* fact: defaults */
      kind_lifecycle_load(db, KIND_FACT, &lc);
      assert(lc.promote_use_count == 3);
      assert(fabs(lc.promote_confidence - 0.9) < 0.01);
      assert(lc.demote_days == 60);
      assert(fabs(lc.demotion_resistance - 1.0) < 0.01);

      /* policy: easy promote, aggressive demotion resistance */
      kind_lifecycle_load(db, KIND_POLICY, &lc);
      assert(lc.promote_use_count == 1);
      assert(fabs(lc.promote_confidence - 0.7) < 0.01);
      assert(lc.demote_days == 365);
      assert(fabs(lc.demotion_resistance - 5.0) < 0.01);
      assert(lc.expire_days == 180);

      /* procedure: 2 uses to promote, 3x demotion resistance */
      kind_lifecycle_load(db, KIND_PROCEDURE, &lc);
      assert(lc.promote_use_count == 2);
      assert(lc.demote_days == 180);
      assert(fabs(lc.demotion_resistance - 3.0) < 0.01);

      /* scratch: aggressive expiry */
      kind_lifecycle_load(db, KIND_SCRATCH, &lc);
      assert(lc.expire_days == 3);
      assert(fabs(lc.demotion_resistance - 0.25) < 0.01);

      /* unknown kind: falls back to fact defaults */
      kind_lifecycle_load(db, "unknown_kind", &lc);
      assert(lc.promote_use_count == PROMOTE_L1_USE_COUNT);
      assert(lc.demote_days == DEMOTE_L2_DAYS);

      teardown(db);
   }

   /* --- policy promotes with 1 use --- */
   {
      sqlite3 *db = setup();
      memory_t m;
      memory_insert(db, TIER_L1, KIND_POLICY, "no-cookies", "Never store session tokens in cookies",
                    0.5, "s1", &m);
      memory_touch(db, m.id); /* 1 touch -> use_count = 2 (insert starts at 1) */

      int promoted = memory_promote(db);
      assert(promoted >= 1);

      memory_t updated;
      memory_get(db, m.id, &updated);
      assert(strcmp(updated.tier, TIER_L2) == 0);
      teardown(db);
   }

   /* --- procedure and policy kinds can be inserted and listed --- */
   {
      sqlite3 *db = setup();
      memory_t m;

      int rc = memory_insert(db, TIER_L1, KIND_PROCEDURE, "debug-cert-auth",
                             "Check CA chain, verify SVID expiry, test with openssl s_client", 0.8,
                             "s1", &m);
      assert(rc == 0);
      assert(strcmp(m.kind, KIND_PROCEDURE) == 0);

      rc = memory_insert(db, TIER_L2, KIND_POLICY, "check-pr-state",
                         "Always check PR merge state before pushing", 0.9, "s1", &m);
      assert(rc == 0);
      assert(strcmp(m.kind, KIND_POLICY) == 0);

      /* Stats should reflect new kinds */
      memory_stats_t stats;
      memory_stats(db, &stats);
      assert(stats.kind_counts[6] == 1); /* procedure */
      assert(stats.kind_counts[7] == 1); /* policy */
      assert(stats.total == 2);

      teardown(db);
   }

   /* --- classify_intent tests --- */
   {
      /* Debug intent */
      assert(classify_intent("fix the crash in auth module") == INTENT_DEBUG);
      assert(classify_intent("debug segfault in parser") == INTENT_DEBUG);
      assert(classify_intent("this error keeps failing") == INTENT_DEBUG);

      /* Plan intent */
      assert(classify_intent("design new API endpoint") == INTENT_PLAN);
      assert(classify_intent("implement user authentication") == INTENT_PLAN);
      assert(classify_intent("add support for webhooks") == INTENT_PLAN);

      /* Review intent */
      assert(classify_intent("review the PR for style issues") == INTENT_REVIEW);
      assert(classify_intent("audit security conventions") == INTENT_REVIEW);

      /* Deploy intent */
      assert(classify_intent("deploy the release to production") == INTENT_DEPLOY);
      assert(classify_intent("migrate the database schema") == INTENT_DEPLOY);

      /* General (no clear intent) */
      assert(classify_intent("hello world") == INTENT_GENERAL);
      assert(classify_intent("") == INTENT_GENERAL);
      assert(classify_intent(NULL) == INTENT_GENERAL);
   }

   /* --- retrieval_plan_for_intent tests --- */
   {
      retrieval_plan_t plan;

      /* Debug: procedures + episodes should dominate */
      retrieval_plan_for_intent(INTENT_DEBUG, &plan);
      assert(plan.include_l3 == 1);
      assert(plan.recency_weight > 0.5);
      assert(plan.kind_budget[6] >= 0.25); /* procedure */
      assert(plan.kind_budget[3] >= 0.20); /* episode */

      /* Plan: facts + decisions + policies should dominate */
      retrieval_plan_for_intent(INTENT_PLAN, &plan);
      assert(plan.include_l3 == 0);
      assert(plan.kind_budget[0] >= 0.20); /* fact */
      assert(plan.kind_budget[2] >= 0.20); /* decision */
      assert(plan.kind_budget[7] >= 0.15); /* policy */

      /* Deploy: should include L3 failure warnings */
      retrieval_plan_for_intent(INTENT_DEPLOY, &plan);
      assert(plan.include_l3 == 1);
      assert(plan.kind_budget[6] >= 0.25); /* procedure */

      /* General: balanced */
      retrieval_plan_for_intent(INTENT_GENERAL, &plan);
      assert(plan.include_l3 == 0);

      /* Budget fractions should sum to ~1.0 for all intents */
      for (int intent = 0; intent <= INTENT_GENERAL; intent++)
      {
         retrieval_plan_for_intent((task_intent_t)intent, &plan);
         double sum = 0;
         for (int k = 0; k < NUM_KINDS; k++)
            sum += plan.kind_budget[k];
         assert(sum > 0.95 && sum < 1.05);
      }
   }

   printf("memory: all tests passed\n");
   return 0;
}
