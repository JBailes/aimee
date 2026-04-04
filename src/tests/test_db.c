#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "aimee.h"

static void test_open_memory(void)
{
   sqlite3 *db = db_open(":memory:");
   assert(db != NULL);

   /* Verify tables exist */
   sqlite3_stmt *stmt;
   int rc =
       sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type='table'", -1, &stmt, NULL);
   assert(rc == SQLITE_OK);

   int table_count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW)
      table_count++;
   sqlite3_finalize(stmt);

   /* Should have many tables from migrations */
   assert(table_count >= 15);

   db_stmt_cache_clear();
   db_close(db);
}

static void test_fts5_available(void)
{
   sqlite3 *db = db_open(":memory:");
   assert(db != NULL);
   assert(db_fts5_available(db) == 1);
   db_stmt_cache_clear();
   db_close(db);
}

static void test_prepare_cache(void)
{
   sqlite3 *db = db_open(":memory:");
   assert(db != NULL);

   sqlite3_stmt *s1 = db_prepare(db, "SELECT 1");
   assert(s1 != NULL);

   /* Same query should return same statement */
   sqlite3_stmt *s2 = db_prepare(db, "SELECT 1");
   assert(s1 == s2);

   db_stmt_cache_clear();
   db_close(db);
}

static void test_migrations_idempotent(void)
{
   sqlite3 *db = db_open(":memory:");
   assert(db != NULL);
   db_stmt_cache_clear();
   db_close(db);

   /* Opening again (migrations already applied) should succeed */
   db = db_open(":memory:");
   assert(db != NULL);
   db_stmt_cache_clear();
   db_close(db);
}

static void test_migration_versions(void)
{
   sqlite3 *db = db_open(":memory:");
   assert(db != NULL);

   /* Verify schema_migrations table has entries */
   sqlite3_stmt *stmt;
   int rc = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM schema_migrations", -1, &stmt, NULL);
   assert(rc == SQLITE_OK);
   assert(sqlite3_step(stmt) == SQLITE_ROW);
   int version_count = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);

   /* Should have at least 27 migration versions */
   assert(version_count >= 27);

   /* Verify versions are sequential from 1 */
   rc = sqlite3_prepare_v2(db, "SELECT MIN(version), MAX(version) FROM schema_migrations", -1,
                           &stmt, NULL);
   assert(rc == SQLITE_OK);
   assert(sqlite3_step(stmt) == SQLITE_ROW);
   int min_ver = sqlite3_column_int(stmt, 0);
   int max_ver = sqlite3_column_int(stmt, 1);
   sqlite3_finalize(stmt);
   assert(min_ver == 1);
   assert(max_ver == version_count);

   db_stmt_cache_clear();
   db_close(db);
}

static void test_key_tables_exist(void)
{
   sqlite3 *db = db_open(":memory:");
   assert(db != NULL);

   /* Verify critical tables from specific migrations */
   const char *required_tables[] = {"rules",          "memories",        "tasks",
                                    "anti_patterns",  "checkpoints",     "agent_log",
                                    "working_memory", "server_sessions", NULL};

   for (int i = 0; required_tables[i]; i++)
   {
      char sql[256];
      snprintf(sql, sizeof(sql), "SELECT 1 FROM sqlite_master WHERE type='table' AND name='%s'",
               required_tables[i]);
      sqlite3_stmt *stmt;
      int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
      assert(rc == SQLITE_OK);
      int exists = (sqlite3_step(stmt) == SQLITE_ROW);
      sqlite3_finalize(stmt);
      assert(exists);
   }

   db_stmt_cache_clear();
   db_close(db);
}

static void test_wal_mode(void)
{
   /* WAL mode should be set on file-backed DBs */
   sqlite3 *db = db_open("/tmp/aimee-test-wal.db");
   assert(db != NULL);

   sqlite3_stmt *stmt;
   int rc = sqlite3_prepare_v2(db, "PRAGMA journal_mode", -1, &stmt, NULL);
   assert(rc == SQLITE_OK);
   assert(sqlite3_step(stmt) == SQLITE_ROW);
   const char *mode = (const char *)sqlite3_column_text(stmt, 0);
   assert(strcmp(mode, "wal") == 0);
   sqlite3_finalize(stmt);

   db_stmt_cache_clear();
   db_close(db);
   unlink("/tmp/aimee-test-wal.db");
   unlink("/tmp/aimee-test-wal.db-wal");
   unlink("/tmp/aimee-test-wal.db-shm");
}

static void test_prepare_cache_different_queries(void)
{
   sqlite3 *db = db_open(":memory:");
   assert(db != NULL);

   sqlite3_stmt *s1 = db_prepare(db, "SELECT 1");
   sqlite3_stmt *s2 = db_prepare(db, "SELECT 2");
   assert(s1 != NULL);
   assert(s2 != NULL);
   assert(s1 != s2); /* Different queries get different statements */

   /* Same query again returns cached */
   sqlite3_stmt *s3 = db_prepare(db, "SELECT 1");
   assert(s3 == s1);

   db_stmt_cache_clear();
   db_close(db);
}

static void test_db_open_fast(void)
{
   /* db_open_fast on already-initialized DB should succeed */
   sqlite3 *db = db_open("/tmp/aimee-test-fast.db");
   assert(db != NULL);
   db_stmt_cache_clear();
   db_close(db);

   db = db_open_fast("/tmp/aimee-test-fast.db");
   assert(db != NULL);
   db_stmt_cache_clear();
   db_close(db);

   unlink("/tmp/aimee-test-fast.db");
   unlink("/tmp/aimee-test-fast.db-wal");
   unlink("/tmp/aimee-test-fast.db-shm");
}

static void test_schema_version_fastpath(void)
{
   const char *path = "/tmp/aimee-test-schema-ver.db";
   unlink(path);

   /* First open: runs all migrations, sets user_version */
   sqlite3 *db = db_open(path);
   assert(db != NULL);

   /* Verify user_version was set to a positive value */
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &stmt, NULL);
   assert(rc == SQLITE_OK);
   assert(sqlite3_step(stmt) == SQLITE_ROW);
   int user_ver = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   assert(user_ver > 0);

   /* Verify it matches the number of migration entries in schema_migrations */
   stmt = NULL;
   rc = sqlite3_prepare_v2(db, "SELECT MAX(version) FROM schema_migrations", -1, &stmt, NULL);
   assert(rc == SQLITE_OK);
   assert(sqlite3_step(stmt) == SQLITE_ROW);
   int max_migration = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   assert(user_ver == max_migration);

   db_stmt_cache_clear();
   db_close(db);

   /* Second open: should hit fast-path (user_version matches).
    * Verify it still works correctly. */
   db = db_open(path);
   assert(db != NULL);

   /* Tables should still be accessible */
   stmt = NULL;
   rc = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM memories", -1, &stmt, NULL);
   assert(rc == SQLITE_OK);
   sqlite3_finalize(stmt);

   db_stmt_cache_clear();
   db_close(db);

   unlink(path);
   char wal[256], shm[256];
   snprintf(wal, sizeof(wal), "%s-wal", path);
   snprintf(shm, sizeof(shm), "%s-shm", path);
   unlink(wal);
   unlink(shm);
}

static void test_schema_version_detects_new_migration(void)
{
   const char *path = "/tmp/aimee-test-schema-new.db";
   unlink(path);

   /* Open to run all migrations */
   sqlite3 *db = db_open(path);
   assert(db != NULL);

   /* Tamper: set user_version to something lower to simulate a new migration added */
   sqlite3_exec(db, "PRAGMA user_version = 1", NULL, NULL, NULL);

   db_stmt_cache_clear();
   db_close(db);

   /* Reopen: should detect mismatch and run through migration loop again */
   db = db_open(path);
   assert(db != NULL);

   /* user_version should be updated back to latest */
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &stmt, NULL);
   assert(sqlite3_step(stmt) == SQLITE_ROW);
   int user_ver = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   assert(user_ver > 1);

   db_stmt_cache_clear();
   db_close(db);

   unlink(path);
   char wal[256], shm[256];
   snprintf(wal, sizeof(wal), "%s-wal", path);
   snprintf(shm, sizeof(shm), "%s-shm", path);
   unlink(wal);
   unlink(shm);
}

static void test_pragma_profile_cli(void)
{
   sqlite3 *db = db_open("/tmp/aimee-test-pragma-cli.db");
   assert(db != NULL);

   /* CLI mode should have synchronous=FULL (2) */
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(db, "PRAGMA synchronous", -1, &stmt, NULL);
   assert(sqlite3_step(stmt) == SQLITE_ROW);
   int sync_val = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   assert(sync_val == 2); /* FULL */

   /* CLI mode should have cache_size=-2048 */
   stmt = NULL;
   sqlite3_prepare_v2(db, "PRAGMA cache_size", -1, &stmt, NULL);
   assert(sqlite3_step(stmt) == SQLITE_ROW);
   int cache = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   assert(cache == -2048);

   /* wal_autocheckpoint should be set */
   stmt = NULL;
   sqlite3_prepare_v2(db, "PRAGMA wal_autocheckpoint", -1, &stmt, NULL);
   assert(sqlite3_step(stmt) == SQLITE_ROW);
   int checkpoint = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   assert(checkpoint == 1000);

   db_stmt_cache_clear();
   db_close(db);
   unlink("/tmp/aimee-test-pragma-cli.db");
   unlink("/tmp/aimee-test-pragma-cli.db-wal");
   unlink("/tmp/aimee-test-pragma-cli.db-shm");
}

static void test_pragma_profile_server(void)
{
   sqlite3 *db = db_open("/tmp/aimee-test-pragma-srv.db");
   assert(db != NULL);

   /* Apply server profile (overrides CLI defaults) */
   db_apply_pragmas(db, DB_MODE_SERVER);

   /* Server mode should have synchronous=NORMAL (1) */
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(db, "PRAGMA synchronous", -1, &stmt, NULL);
   assert(sqlite3_step(stmt) == SQLITE_ROW);
   int sync_val = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   assert(sync_val == 1); /* NORMAL */

   /* Server mode should have cache_size=-8192 */
   stmt = NULL;
   sqlite3_prepare_v2(db, "PRAGMA cache_size", -1, &stmt, NULL);
   assert(sqlite3_step(stmt) == SQLITE_ROW);
   int cache = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   assert(cache == -8192);

   /* Server mode should have mmap_size=67108864 */
   stmt = NULL;
   sqlite3_prepare_v2(db, "PRAGMA mmap_size", -1, &stmt, NULL);
   assert(sqlite3_step(stmt) == SQLITE_ROW);
   long long mmap = sqlite3_column_int64(stmt, 0);
   sqlite3_finalize(stmt);
   assert(mmap == 67108864);

   db_stmt_cache_clear();
   db_close(db);
   unlink("/tmp/aimee-test-pragma-srv.db");
   unlink("/tmp/aimee-test-pragma-srv.db-wal");
   unlink("/tmp/aimee-test-pragma-srv.db-shm");
}

static void test_migration_ordering_validation(void)
{
   char err[256] = "";
   int rc = db_validate_migrations(err, sizeof(err));
   assert(rc == 0); /* migrations must be strictly ordered with no non-contiguous duplicates */

   int next = db_next_migration_version();
   assert(next > 0); /* next version must be positive */
}

int main(void)
{
   test_open_memory();
   test_fts5_available();
   test_prepare_cache();
   test_prepare_cache_different_queries();
   test_migrations_idempotent();
   test_migration_versions();
   test_migration_ordering_validation();
   test_key_tables_exist();
   test_wal_mode();
   test_db_open_fast();
   test_schema_version_fastpath();
   test_schema_version_detects_new_migration();
   test_pragma_profile_cli();
   test_pragma_profile_server();
   printf("db: all tests passed\n");
   return 0;
}
