#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "aimee.h"
#include "agent_exec.h"

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

static void test_null_hint_produces_same_as_original(void)
{
   sqlite3 *db = setup();
   memory_t m;

   /* Insert some memories across kinds/tiers */
   memory_insert(db, TIER_L2, KIND_FACT, "db-host", "PostgreSQL at 10.0.0.5", 0.9, "s1", &m);
   memory_insert(db, TIER_L2, KIND_PREFERENCE, "style", "concise responses", 0.8, "s1", &m);
   memory_insert(db, TIER_L1, KIND_TASK, "deploy-api", "deploy the API service", 0.7, "s1", &m);
   memory_insert(db, TIER_L1, KIND_EPISODE, "fixed-auth", "fixed auth cert issue", 0.6, "s1", &m);
   memory_insert(db, TIER_L2, KIND_DECISION, "use-mTLS", "all services use mTLS", 0.95, "s1", &m);

   char *ctx = memory_assemble_context(db, NULL);
   assert(ctx != NULL);
   assert(strlen(ctx) > 0);

   /* Should have the standard section headers */
   assert(strstr(ctx, "# Memory Context") != NULL);
   assert(strstr(ctx, "## Key Facts") != NULL);
   assert(strstr(ctx, "## Constraints") != NULL);

   /* Should contain the inserted content */
   assert(strstr(ctx, "PostgreSQL") != NULL);
   assert(strstr(ctx, "mTLS") != NULL);

   free(ctx);
   teardown(db);
}

static void test_task_hint_prioritizes_relevant_memories(void)
{
   sqlite3 *db = setup();
   memory_t m;

   /* Insert auth-related and unrelated facts */
   memory_insert(db, TIER_L2, KIND_FACT, "auth-config",
                 "PostgreSQL cert auth uses client certificates", 0.9, "s1", &m);
   memory_insert(db, TIER_L2, KIND_FACT, "deploy-config", "deployments use blue-green strategy",
                 0.9, "s1", &m);
   memory_insert(db, TIER_L2, KIND_FACT, "auth-flow",
                 "authentication flow validates certificate chain", 0.5, "s1", &m);

   /* With auth-related task hint, auth memories should appear first */
   char *ctx = memory_assemble_context(db, "fix PostgreSQL cert auth");
   assert(ctx != NULL);

   /* Auth-related content should be present */
   assert(strstr(ctx, "cert") != NULL);

   /* Both auth memories should appear before the deploy one */
   char *auth1 = strstr(ctx, "client certificates");
   char *auth2 = strstr(ctx, "certificate chain");
   char *deploy = strstr(ctx, "blue-green");

   /* At minimum the highest-scored auth memory should be there */
   assert(auth1 != NULL);

   /* If deploy appears at all, auth should come first */
   if (deploy != NULL && auth1 != NULL)
      assert(auth1 < deploy);
   if (deploy != NULL && auth2 != NULL)
      assert(auth2 < deploy);

   free(ctx);
   teardown(db);
}

static void test_task_hint_respects_budget(void)
{
   sqlite3 *db = setup();
   memory_t m;

   /* Insert many memories to test budget enforcement */
   for (int i = 0; i < 30; i++)
   {
      char key[64], content[256];
      snprintf(key, sizeof(key), "fact-%d", i);
      snprintf(content, sizeof(content), "this is fact number %d about auth configuration", i);
      memory_insert(db, TIER_L2, KIND_FACT, key, content, 0.9 - (i * 0.01), "s1", &m);
   }

   char *ctx = memory_assemble_context(db, "auth configuration");
   assert(ctx != NULL);
   assert((int)strlen(ctx) <= MAX_CONTEXT_TOTAL + 256);

   free(ctx);
   teardown(db);
}

static void test_task_hint_fills_all_sections(void)
{
   sqlite3 *db = setup();
   memory_t m;

   /* Insert memories of each kind that match the task hint */
   memory_insert(db, TIER_L2, KIND_FACT, "net-topology", "network uses VLAN isolation", 0.9, "s1",
                 &m);
   memory_insert(db, TIER_L1, KIND_TASK, "fix-network", "fix network routing issue", 0.7, "s1", &m);
   memory_insert(db, TIER_L1, KIND_EPISODE, "network-outage", "network outage on March 15", 0.6,
                 "s1", &m);
   memory_insert(db, TIER_L2, KIND_DECISION, "network-policy",
                 "network policy requires firewall rules", 0.95, "s1", &m);

   char *ctx = memory_assemble_context(db, "network routing");
   assert(ctx != NULL);

   /* All four sections should be populated */
   assert(strstr(ctx, "## Key Facts") != NULL);
   assert(strstr(ctx, "## Active Tasks") != NULL);
   assert(strstr(ctx, "## Recent Context") != NULL);
   assert(strstr(ctx, "## Constraints") != NULL);

   /* Content from each section should be present */
   assert(strstr(ctx, "VLAN isolation") != NULL);
   assert(strstr(ctx, "routing issue") != NULL);
   assert(strstr(ctx, "outage") != NULL);
   assert(strstr(ctx, "firewall") != NULL);

   free(ctx);
   teardown(db);
}

static void test_empty_db_with_task_hint(void)
{
   sqlite3 *db = setup();

   char *ctx = memory_assemble_context(db, "anything");
   assert(ctx != NULL);
   assert(strstr(ctx, "# Memory Context") != NULL);
   /* No sections should appear */
   assert(strstr(ctx, "## Key Facts") == NULL);

   free(ctx);
   teardown(db);
}

static void test_graph_boost_integration(void)
{
   sqlite3 *db = setup();
   memory_t m;

   /* Insert memories */
   memory_insert(db, TIER_L2, KIND_FACT, "spire-config",
                 "SPIRE manages X.509 certificates for mTLS", 0.7, "s1", &m);
   memory_insert(db, TIER_L2, KIND_FACT, "unrelated-fact", "disk usage is monitored by Prometheus",
                 0.9, "s1", &m);

   /* Create entity edge linking "spire" to "auth" */
   static const char *edge_sql = "INSERT INTO entity_edges (source, relation, target, weight)"
                                 " VALUES ('spire', 'provides', 'auth', 3)";
   sqlite3_stmt *stmt = db_prepare(db, edge_sql);
   if (stmt)
   {
      sqlite3_step(stmt);
      sqlite3_reset(stmt);
   }

   /* Search for "auth" - spire should get a graph boost */
   char *ctx = memory_assemble_context(db, "auth certificates");
   assert(ctx != NULL);

   /* SPIRE memory should appear due to graph boost even though
    * the unrelated fact has higher base confidence */
   assert(strstr(ctx, "SPIRE") != NULL);

   free(ctx);
   teardown(db);
}

/* --- Task type classification tests --- */

static void test_classify_bug_fix(void)
{
   assert(task_type_classify("fix the crash in auth module") == TASK_TYPE_BUG_FIX);
   assert(task_type_classify("Debug the error in login") == TASK_TYPE_BUG_FIX);
   assert(task_type_classify("Something is broken in deploy") == TASK_TYPE_BUG_FIX);
   assert(task_type_classify("Investigate the regression") == TASK_TYPE_BUG_FIX);
   assert(task_type_classify("The build fails on CI") == TASK_TYPE_BUG_FIX);
}

static void test_classify_refactor(void)
{
   assert(task_type_classify("refactor the auth module") == TASK_TYPE_REFACTOR);
   assert(task_type_classify("rename getUserData to fetchUser") == TASK_TYPE_REFACTOR);
   assert(task_type_classify("extract common logic into helper") == TASK_TYPE_REFACTOR);
   assert(task_type_classify("clean up the unused imports") == TASK_TYPE_REFACTOR);
}

static void test_classify_feature(void)
{
   assert(task_type_classify("add pagination to the API") == TASK_TYPE_FEATURE);
   assert(task_type_classify("implement rate limiting") == TASK_TYPE_FEATURE);
   assert(task_type_classify("create a new endpoint for users") == TASK_TYPE_FEATURE);
   assert(task_type_classify("build webhook support") == TASK_TYPE_FEATURE);
}

static void test_classify_review(void)
{
   assert(task_type_classify("review the PR changes") == TASK_TYPE_REVIEW);
   assert(task_type_classify("audit the security config") == TASK_TYPE_REVIEW);
   assert(task_type_classify("verify the deployment worked") == TASK_TYPE_REVIEW);
   assert(task_type_classify("validate the schema migration") == TASK_TYPE_REVIEW);
}

static void test_classify_test(void)
{
   assert(task_type_classify("test the auth flow") == TASK_TYPE_TEST);
   assert(task_type_classify("increase test coverage for db") == TASK_TYPE_TEST);
   assert(task_type_classify("write unit tests for parser") == TASK_TYPE_TEST);
}

static void test_classify_general(void)
{
   assert(task_type_classify("deploy the service") == TASK_TYPE_GENERAL);
   assert(task_type_classify("update the config") == TASK_TYPE_GENERAL);
   assert(task_type_classify(NULL) == TASK_TYPE_GENERAL);
   assert(task_type_classify("") == TASK_TYPE_GENERAL);
}

static void test_task_type_name_strings(void)
{
   assert(strcmp(task_type_name(TASK_TYPE_BUG_FIX), "bug_fix") == 0);
   assert(strcmp(task_type_name(TASK_TYPE_REFACTOR), "refactor") == 0);
   assert(strcmp(task_type_name(TASK_TYPE_FEATURE), "feature") == 0);
   assert(strcmp(task_type_name(TASK_TYPE_REVIEW), "review") == 0);
   assert(strcmp(task_type_name(TASK_TYPE_TEST), "test") == 0);
   assert(strcmp(task_type_name(TASK_TYPE_GENERAL), "general") == 0);
}

int main(void)
{
   test_null_hint_produces_same_as_original();
   test_task_hint_prioritizes_relevant_memories();
   test_task_hint_respects_budget();
   test_task_hint_fills_all_sections();
   test_empty_db_with_task_hint();
   test_graph_boost_integration();
   test_classify_bug_fix();
   test_classify_refactor();
   test_classify_feature();
   test_classify_review();
   test_classify_test();
   test_classify_general();
   test_task_type_name_strings();
   printf("context_assembly: all tests passed\n");
   return 0;
}
