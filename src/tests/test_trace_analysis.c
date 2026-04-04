#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "aimee.h"
#include "trace_analysis.h"

static void insert_trace(sqlite3 *db, int plan_id, int turn, const char *tool_name,
                         const char *tool_result)
{
   sqlite3_stmt *stmt = db_prepare(db, "INSERT INTO execution_trace "
                                       "(plan_id, turn, direction, content, tool_name, tool_args, "
                                       "tool_result) VALUES (?, ?, 'call', '', ?, '{}', ?)");
   assert(stmt != NULL);
   sqlite3_bind_int(stmt, 1, plan_id);
   sqlite3_bind_int(stmt, 2, turn);
   sqlite3_bind_text(stmt, 3, tool_name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, tool_result, -1, SQLITE_TRANSIENT);
   assert(sqlite3_step(stmt) == SQLITE_DONE);
   sqlite3_reset(stmt);
}

static int count_rows(sqlite3 *db, const char *sql)
{
   sqlite3_stmt *stmt = db_prepare(db, sql);
   assert(stmt != NULL);
   assert(sqlite3_step(stmt) == SQLITE_ROW);
   int count = sqlite3_column_int(stmt, 0);
   sqlite3_reset(stmt);
   return count;
}

static void test_retry_loops_and_duplicates(sqlite3 *db)
{
   insert_trace(db, 1, 1, "bash", "error: failed");
   insert_trace(db, 1, 2, "bash", "failed again");
   insert_trace(db, 1, 3, "bash", "ERROR final");
   assert(trace_mine(db) >= 1);
   assert(count_rows(db, "SELECT COUNT(*) FROM anti_patterns") == 1);
   assert(count_rows(db, "SELECT COUNT(*) FROM trace_mining_log") == 1);

   assert(trace_mine(db) == 0);
   assert(count_rows(db, "SELECT COUNT(*) FROM anti_patterns") == 1);
   assert(count_rows(db, "SELECT COUNT(*) FROM trace_mining_log") == 1);
}

static void test_recovery_and_incremental_mining(sqlite3 *db)
{
   insert_trace(db, 2, 1, "grep", "No such file");
   insert_trace(db, 2, 2, "find", "ok");
   assert(trace_mine(db) >= 1);
   assert(count_rows(db, "SELECT COUNT(*) FROM memories WHERE key = 'recovery:grep->find'") == 1);
   assert(count_rows(db, "SELECT COUNT(*) FROM trace_mining_log") == 2);

   insert_trace(db, 3, 1, "grep", "not found");
   insert_trace(db, 3, 2, "find", "ok");
   assert(trace_mine(db) == 0);
   assert(count_rows(db, "SELECT COUNT(*) FROM memories WHERE key = 'recovery:grep->find'") == 1);
   assert(count_rows(db, "SELECT COUNT(*) FROM trace_mining_log") == 3);
}

static void test_common_sequence(sqlite3 *db)
{
   insert_trace(db, 10, 1, "scan", "ok");
   insert_trace(db, 10, 2, "summarize", "ok");
   insert_trace(db, 11, 1, "scan", "ok");
   insert_trace(db, 11, 2, "summarize", "ok");
   insert_trace(db, 12, 1, "scan", "ok");
   insert_trace(db, 12, 2, "summarize", "ok");
   assert(trace_mine(db) >= 1);
   assert(count_rows(db, "SELECT COUNT(*) FROM memories WHERE key = 'sequence:scan->summarize'") ==
          1);
}

int main(void)
{
   sqlite3 *db = db_open(":memory:");
   assert(db != NULL);
   test_retry_loops_and_duplicates(db);
   test_recovery_and_incremental_mining(db);
   test_common_sequence(db);
   db_stmt_cache_clear();
   db_close(db);
   printf("trace_analysis: all tests passed\n");
   return 0;
}
