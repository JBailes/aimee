#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "aimee.h"

static char tmpdir[64];

static sqlite3 *setup(void)
{
   /* Isolate from real config so auto_tag_workspace doesn't pick up cwd workspaces */
   snprintf(tmpdir, sizeof(tmpdir), "/tmp/aimee-test-wsmem-XXXXXX");
   assert(mkdtemp(tmpdir) != NULL);
   setenv("HOME", tmpdir, 1);

   sqlite3 *db = db_open(":memory:");
   assert(db != NULL);
   return db;
}

static void teardown(sqlite3 *db)
{
   db_stmt_cache_clear();
   db_close(db);
   char cmd[256];
   snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
   (void)system(cmd);
}

static void test_tag_workspace(void)
{
   sqlite3 *db = setup();
   memory_t m;
   memory_insert(db, TIER_L2, KIND_FACT, "db-host", "host at 10.0.0.5", 0.9, "s1", &m);

   int rc = memory_tag_workspace(db, m.id, "wol");
   assert(rc == 0);

   /* Verify tag exists */
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(db,
                      "SELECT COUNT(*) FROM memory_workspaces"
                      " WHERE memory_id = ? AND workspace = 'wol'",
                      -1, &stmt, NULL);
   sqlite3_bind_int64(stmt, 1, m.id);
   assert(sqlite3_step(stmt) == SQLITE_ROW);
   assert(sqlite3_column_int(stmt, 0) == 1);
   sqlite3_finalize(stmt);

   teardown(db);
}

static void test_tag_multiple_workspaces(void)
{
   sqlite3 *db = setup();
   memory_t m;
   memory_insert(db, TIER_L2, KIND_FACT, "shared-fact", "network topology", 0.9, "s1", &m);

   memory_tag_workspace(db, m.id, "wol");
   memory_tag_workspace(db, m.id, "infrastructure");
   memory_tag_workspace(db, m.id, SHARED_WORKSPACE);

   /* Count tags */
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM memory_workspaces WHERE memory_id = ?", -1, &stmt,
                      NULL);
   sqlite3_bind_int64(stmt, 1, m.id);
   assert(sqlite3_step(stmt) == SQLITE_ROW);
   assert(sqlite3_column_int(stmt, 0) == 3);
   sqlite3_finalize(stmt);

   teardown(db);
}

static void test_tag_idempotent(void)
{
   sqlite3 *db = setup();
   memory_t m;
   memory_insert(db, TIER_L2, KIND_FACT, "key1", "content", 0.9, "s1", &m);

   /* Tag same workspace twice — should not duplicate */
   memory_tag_workspace(db, m.id, "wol");
   memory_tag_workspace(db, m.id, "wol");

   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM memory_workspaces WHERE memory_id = ?", -1, &stmt,
                      NULL);
   sqlite3_bind_int64(stmt, 1, m.id);
   assert(sqlite3_step(stmt) == SQLITE_ROW);
   assert(sqlite3_column_int(stmt, 0) == 1);
   sqlite3_finalize(stmt);

   teardown(db);
}

static void test_cascade_delete(void)
{
   sqlite3 *db = setup();
   memory_t m;
   memory_insert(db, TIER_L2, KIND_FACT, "deleteme", "content", 0.9, "s1", &m);
   memory_tag_workspace(db, m.id, "wol");

   memory_delete(db, m.id);

   /* Tags should be gone due to CASCADE */
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM memory_workspaces WHERE memory_id = ?", -1, &stmt,
                      NULL);
   sqlite3_bind_int64(stmt, 1, m.id);
   assert(sqlite3_step(stmt) == SQLITE_ROW);
   assert(sqlite3_column_int(stmt, 0) == 0);
   sqlite3_finalize(stmt);

   teardown(db);
}

static void test_ws_context_scoped_memories_first(void)
{
   sqlite3 *db = setup();
   memory_t m;

   /* Insert a wol-scoped fact */
   memory_insert(db, TIER_L2, KIND_FACT, "wol-config", "WOL uses mTLS on port 8443", 0.9, "s1", &m);
   memory_tag_workspace(db, m.id, "wol");

   /* Insert an aimee-scoped fact */
   memory_insert(db, TIER_L2, KIND_FACT, "aimee-config", "aimee uses SQLite for storage", 0.9, "s1",
                 &m);
   memory_tag_workspace(db, m.id, "aimee");

   /* Insert a shared fact */
   memory_insert(db, TIER_L2, KIND_FACT, "infra-fact", "all services run on Proxmox VE", 0.9, "s1",
                 &m);
   memory_tag_workspace(db, m.id, SHARED_WORKSPACE);

   /* Assemble context for "wol" workspace */
   char *ctx = memory_assemble_context_ws(db, NULL, "wol");
   assert(ctx != NULL);

   /* wol-scoped memory should be present */
   assert(strstr(ctx, "mTLS") != NULL);

   /* shared memory should be present */
   assert(strstr(ctx, "Proxmox") != NULL);

   /* aimee-scoped memory should NOT be in main sections (only in cross-workspace if high enough) */
   /* Since we don't set use_count>=5, it shouldn't appear in cross-workspace section either */
   char *aimee_ref = strstr(ctx, "SQLite for storage");
   assert(aimee_ref == NULL);

   free(ctx);
   teardown(db);
}

static void test_ws_untagged_treated_as_shared(void)
{
   sqlite3 *db = setup();
   memory_t m;

   /* Insert an untagged legacy memory */
   memory_insert(db, TIER_L2, KIND_FACT, "legacy-fact", "legacy untagged memory content", 0.9, "s1",
                 &m);
   /* Don't tag it — should be treated as _shared */

   char *ctx = memory_assemble_context_ws(db, NULL, "wol");
   assert(ctx != NULL);

   /* Untagged memory should appear (treated as _shared) */
   assert(strstr(ctx, "legacy untagged") != NULL);

   free(ctx);
   teardown(db);
}

static void test_ws_cross_workspace_high_confidence(void)
{
   sqlite3 *db = setup();
   memory_t m;

   /* Insert a high-confidence memory in another workspace */
   memory_insert(db, TIER_L2, KIND_FACT, "other-ws-fact", "critical pattern from other project",
                 0.95, "s1", &m);
   memory_tag_workspace(db, m.id, "other-project");

   /* Bump use_count to >= 5 */
   for (int i = 0; i < 5; i++)
      memory_touch(db, m.id);

   char *ctx = memory_assemble_context_ws(db, NULL, "wol");
   assert(ctx != NULL);

   /* High-confidence cross-workspace memory should appear in Cross-Workspace section */
   assert(strstr(ctx, "Cross-Workspace") != NULL);
   assert(strstr(ctx, "critical pattern") != NULL);

   free(ctx);
   teardown(db);
}

static void test_ws_null_workspace_falls_back(void)
{
   sqlite3 *db = setup();
   memory_t m;
   memory_insert(db, TIER_L2, KIND_FACT, "any-fact", "some content", 0.9, "s1", &m);

   /* NULL workspace should fall back to regular assembly */
   char *ctx = memory_assemble_context_ws(db, NULL, NULL);
   assert(ctx != NULL);
   assert(strstr(ctx, "some content") != NULL);

   free(ctx);
   teardown(db);
}

static void test_auto_tag_shared_keywords(void)
{
   sqlite3 *db = setup();
   memory_t m;

   /* This memory mentions "auth" — should be auto-tagged _shared */
   memory_insert(db, TIER_L2, KIND_FACT, "auth-config", "PostgreSQL cert auth flow", 0.9, "s1", &m);

   /* auto_tag was called during insert, check for _shared tag */
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(db,
                      "SELECT COUNT(*) FROM memory_workspaces"
                      " WHERE memory_id = ? AND workspace = ?",
                      -1, &stmt, NULL);
   sqlite3_bind_int64(stmt, 1, m.id);
   sqlite3_bind_text(stmt, 2, SHARED_WORKSPACE, -1, SQLITE_TRANSIENT);
   assert(sqlite3_step(stmt) == SQLITE_ROW);
   int count = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);

   /* Should have _shared tag because "auth" is a shared keyword */
   assert(count == 1);

   teardown(db);
}

int main(void)
{
   test_tag_workspace();
   test_tag_multiple_workspaces();
   test_tag_idempotent();
   test_cascade_delete();
   test_ws_context_scoped_memories_first();
   test_ws_untagged_treated_as_shared();
   test_ws_cross_workspace_high_confidence();
   test_ws_null_workspace_falls_back();
   test_auto_tag_shared_keywords();
   printf("workspace_memory: all tests passed\n");
   return 0;
}
