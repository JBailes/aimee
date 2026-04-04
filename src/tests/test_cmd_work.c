/* test_cmd_work.c: work queue command behavior tests for cmd_work.c */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "aimee.h"
#include "commands.h"
#include "cJSON.h"

/* --- Test get_work_subcmds returns valid table --- */

static void test_work_subcmds_table(void)
{
   const subcmd_t *table = get_work_subcmds();
   assert(table != NULL);

   /* Must have at least the known subcommands */
   int found_add = 0, found_claim = 0, found_list = 0, found_complete = 0;
   int count = 0;
   for (int i = 0; table[i].name != NULL; i++)
   {
      assert(table[i].help != NULL);
      assert(table[i].handler != NULL);
      if (strcmp(table[i].name, "add") == 0)
         found_add = 1;
      if (strcmp(table[i].name, "claim") == 0)
         found_claim = 1;
      if (strcmp(table[i].name, "list") == 0)
         found_list = 1;
      if (strcmp(table[i].name, "complete") == 0)
         found_complete = 1;
      count++;
   }
   assert(found_add);
   assert(found_claim);
   assert(found_list);
   assert(found_complete);
   assert(count >= 4);
}

/* --- Test work_queue_summary with empty DB --- */

static void test_work_queue_summary_empty(void)
{
   /* Open a temporary database and create the work_queue table */
   char tmpdb[] = "/tmp/aimee-test-work-XXXXXX";
   int fd = mkstemp(tmpdb);
   assert(fd >= 0);
   close(fd);

   sqlite3 *db = NULL;
   assert(sqlite3_open(tmpdb, &db) == SQLITE_OK);

   /* Create the work_queue table matching the production schema */
   const char *ddl = "CREATE TABLE IF NOT EXISTS work_queue ("
                     "  id TEXT PRIMARY KEY,"
                     "  title TEXT NOT NULL,"
                     "  description TEXT DEFAULT '',"
                     "  source TEXT DEFAULT '',"
                     "  priority INTEGER DEFAULT 0,"
                     "  status TEXT DEFAULT 'pending',"
                     "  created_by TEXT DEFAULT '',"
                     "  claimed_by TEXT DEFAULT '',"
                     "  created_at TEXT DEFAULT '',"
                     "  claimed_at TEXT DEFAULT '',"
                     "  effort TEXT DEFAULT '',"
                     "  tags TEXT DEFAULT ''"
                     ")";
   assert(sqlite3_exec(db, ddl, NULL, NULL, NULL) == SQLITE_OK);

   /* Empty queue: summary should return 0 */
   char buf[1024];
   int len = work_queue_summary(db, buf, sizeof(buf));
   assert(len == 0);

   sqlite3_close(db);
   unlink(tmpdb);
}

/* --- Test work_queue_summary with items --- */

static void test_work_queue_summary_with_items(void)
{
   char tmpdb[] = "/tmp/aimee-test-work2-XXXXXX";
   int fd = mkstemp(tmpdb);
   assert(fd >= 0);
   close(fd);

   sqlite3 *db = NULL;
   assert(sqlite3_open(tmpdb, &db) == SQLITE_OK);

   const char *ddl = "CREATE TABLE IF NOT EXISTS work_queue ("
                     "  id TEXT PRIMARY KEY,"
                     "  title TEXT NOT NULL,"
                     "  description TEXT DEFAULT '',"
                     "  source TEXT DEFAULT '',"
                     "  priority INTEGER DEFAULT 0,"
                     "  status TEXT DEFAULT 'pending',"
                     "  created_by TEXT DEFAULT '',"
                     "  claimed_by TEXT DEFAULT '',"
                     "  created_at TEXT DEFAULT '',"
                     "  claimed_at TEXT DEFAULT '',"
                     "  effort TEXT DEFAULT '',"
                     "  tags TEXT DEFAULT ''"
                     ")";
   assert(sqlite3_exec(db, ddl, NULL, NULL, NULL) == SQLITE_OK);

   /* Insert some pending and claimed items */
   assert(sqlite3_exec(
              db, "INSERT INTO work_queue (id, title, status) VALUES ('a', 'Task A', 'pending')",
              NULL, NULL, NULL) == SQLITE_OK);
   assert(sqlite3_exec(
              db, "INSERT INTO work_queue (id, title, status) VALUES ('b', 'Task B', 'pending')",
              NULL, NULL, NULL) == SQLITE_OK);
   assert(sqlite3_exec(
              db, "INSERT INTO work_queue (id, title, status) VALUES ('c', 'Task C', 'claimed')",
              NULL, NULL, NULL) == SQLITE_OK);
   assert(sqlite3_exec(db,
                       "INSERT INTO work_queue (id, title, status) VALUES ('d', 'Task D', 'done')",
                       NULL, NULL, NULL) == SQLITE_OK);

   char buf[1024];
   int len = work_queue_summary(db, buf, sizeof(buf));
   assert(len > 0);
   /* Should mention pending count */
   assert(strstr(buf, "2 pending") != NULL);
   /* Should mention claimed count */
   assert(strstr(buf, "claimed") != NULL);
   /* Should NOT mention done items */
   assert(strstr(buf, "done") == NULL || strstr(buf, "Task D") == NULL);

   sqlite3_close(db);
   unlink(tmpdb);
}

/* --- Test subcmd_dispatch with work table --- */

static void test_subcmd_dispatch_unknown(void)
{
   const subcmd_t *table = get_work_subcmds();

   /* Dispatching an unknown subcommand should return -1 */
   app_ctx_t ctx;
   memset(&ctx, 0, sizeof(ctx));

   int rc = subcmd_dispatch(table, "nonexistent_subcmd", &ctx, NULL, 0, NULL);
   assert(rc == -1);
}

static void test_subcmd_dispatch_known(void)
{
   const subcmd_t *table = get_work_subcmds();

   /* Verify the table contains recognized names */
   int found = 0;
   for (int i = 0; table[i].name != NULL; i++)
   {
      if (strcmp(table[i].name, "stats") == 0 || strcmp(table[i].name, "gc") == 0 ||
          strcmp(table[i].name, "cancel") == 0 || strcmp(table[i].name, "release") == 0 ||
          strcmp(table[i].name, "fail") == 0)
         found++;
   }
   /* Should have at least these additional subcommands */
   assert(found >= 3);
}

int main(void)
{
   printf("cmd_work: ");

   test_work_subcmds_table();
   test_work_queue_summary_empty();
   test_work_queue_summary_with_items();
   test_subcmd_dispatch_unknown();
   test_subcmd_dispatch_known();

   printf("all tests passed\n");
   return 0;
}
