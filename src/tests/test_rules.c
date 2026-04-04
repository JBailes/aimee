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

static void test_insert_and_list(void)
{
   sqlite3 *db = setup();
   char now[32];
   now_utc(now, sizeof(now));

   sqlite3_exec(db,
                "INSERT INTO rules (polarity, title, description, weight, created_at, updated_at) "
                "VALUES ('positive', 'Test Rule', 'A test', 75, '2025-01-01', '2025-01-01')",
                NULL, NULL, NULL);

   rule_t rules[10];
   int n = rules_list(db, rules, 10);
   assert(n == 1);
   assert(strcmp(rules[0].polarity, "positive") == 0);
   assert(rules[0].weight == 75);

   teardown(db);
}

static void test_find_by_title(void)
{
   sqlite3 *db = setup();
   sqlite3_exec(db,
                "INSERT INTO rules (polarity, title, description, weight, created_at, updated_at) "
                "VALUES ('negative', 'No Force Push', 'Avoid force push', 80, '2025-01-01', "
                "'2025-01-01')",
                NULL, NULL, NULL);

   rule_t r;
   int rc = rules_find_by_title(db, "no force push", &r);
   assert(rc == 0);
   assert(r.weight == 80);

   rc = rules_find_by_title(db, "nonexistent", &r);
   assert(rc == -1);

   teardown(db);
}

static void test_generate(void)
{
   sqlite3 *db = setup();
   sqlite3_exec(db,
                "INSERT INTO rules (polarity, title, description, weight, created_at, updated_at) "
                "VALUES ('positive', 'Rule A', 'Do this', 80, '2025-01-01', '2025-01-01')",
                NULL, NULL, NULL);
   sqlite3_exec(db,
                "INSERT INTO rules (polarity, title, description, weight, created_at, updated_at) "
                "VALUES ('negative', 'Rule B', 'Avoid that', 60, '2025-01-01', '2025-01-01')",
                NULL, NULL, NULL);

   char *md = rules_generate(db);
   assert(md != NULL);
   assert(strstr(md, "# Rules") != NULL);
   assert(strstr(md, "Do this") != NULL);
   assert(strstr(md, "Avoid that") != NULL);
   free(md);

   teardown(db);
}

static void test_tier_label(void)
{
   assert(strcmp(rules_tier(80), "Rule") == 0);
   assert(strcmp(rules_tier(60), "Inclination") == 0);
   assert(strcmp(rules_tier(30), "Archived") == 0);
}

static void test_polarity_symbol(void)
{
   assert(rules_polarity_symbol("positive") == '+');
   assert(rules_polarity_symbol("negative") == '-');
   assert(rules_polarity_symbol("principle") == '~');
}

int main(void)
{
   test_insert_and_list();
   test_find_by_title();
   test_generate();
   test_tier_label();
   test_polarity_symbol();
   printf("rules: all tests passed\n");
   return 0;
}
