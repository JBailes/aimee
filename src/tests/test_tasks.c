#include <assert.h>
#include <stdio.h>
#include <string.h>
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

static void test_create_task(void)
{
   sqlite3 *db = setup();
   aimee_task_t t;
   int rc = aimee_task_create(db, "Implement auth", "s1", 0, &t);
   assert(rc == 0);
   assert(strcmp(t.title, "Implement auth") == 0);
   assert(strcmp(t.state, TASK_TODO) == 0);
   teardown(db);
}

static void test_update_state(void)
{
   sqlite3 *db = setup();
   aimee_task_t t;
   aimee_task_create(db, "Test task", "s1", 0, &t);
   task_update_state(db, t.id, TASK_IN_PROGRESS);

   aimee_task_t updated;
   aimee_task_get(db, t.id, &updated);
   assert(strcmp(updated.state, TASK_IN_PROGRESS) == 0);
   teardown(db);
}

static void test_subtasks(void)
{
   sqlite3 *db = setup();
   aimee_task_t parent, child;
   aimee_task_create(db, "Parent", "s1", 0, &parent);
   aimee_task_create(db, "Child", "s1", parent.id, &child);

   aimee_task_t subs[10];
   int n = aimee_task_get_subtasks(db, parent.id, subs, 10);
   assert(n == 1);
   assert(strcmp(subs[0].title, "Child") == 0);
   teardown(db);
}

static void test_task_edges(void)
{
   sqlite3 *db = setup();
   aimee_task_t t1, t2;
   aimee_task_create(db, "Design", "s1", 0, &t1);
   aimee_task_create(db, "Implement", "s1", 0, &t2);

   int rc = task_add_edge(db, t2.id, t1.id, "depends_on");
   assert(rc == 0);

   task_edge_t edges[10];
   int n = task_get_edges(db, t2.id, edges, 10);
   assert(n == 1);
   assert(strcmp(edges[0].relation, "depends_on") == 0);
   teardown(db);
}

static void test_decision_log(void)
{
   sqlite3 *db = setup();
   decision_t d;
   int rc = decision_log(db, "A, B", "A", "simpler", "API stable", 0, &d);
   assert(rc == 0);
   assert(strcmp(d.chosen, "A") == 0);

   decision_record_outcome(db, d.id, "success");
   decision_t updated;
   decision_get(db, d.id, &updated);
   assert(strcmp(updated.outcome, "success") == 0);
   teardown(db);
}

static void test_checkpoint(void)
{
   sqlite3 *db = setup();
   checkpoint_t cp;
   int rc = checkpoint_create(db, "Before refactor", "s1", 0, &cp);
   assert(rc == 0);
   assert(strcmp(cp.label, "Before refactor") == 0);
   assert(strlen(cp.snapshot) > 0);

   checkpoint_t list[10];
   int n = checkpoint_list(db, 10, list, 10);
   assert(n == 1);

   checkpoint_delete(db, cp.id);
   n = checkpoint_list(db, 10, list, 10);
   assert(n == 0);
   teardown(db);
}

static void test_active_task(void)
{
   sqlite3 *db = setup();
   aimee_task_t t;
   aimee_task_create(db, "Active task", "sess-1", 0, &t);
   task_update_state(db, t.id, TASK_IN_PROGRESS);

   int64_t active = task_get_active(db, "sess-1");
   assert(active == t.id);

   int64_t none = task_get_active(db, "other-session");
   assert(none == 0);
   teardown(db);
}

int main(void)
{
   test_create_task();
   test_update_state();
   test_subtasks();
   test_task_edges();
   test_decision_log();
   test_checkpoint();
   test_active_task();
   printf("tasks: all tests passed\n");
   return 0;
}
