#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "aimee.h"

int main(void)
{
   printf("feedback: ");

   /* --- feedback_parse_polarity --- */
   {
      assert(strcmp(feedback_parse_polarity("+"), "positive") == 0);
      assert(strcmp(feedback_parse_polarity("-"), "negative") == 0);
      assert(strcmp(feedback_parse_polarity("positive"), "positive") == 0);
      assert(strcmp(feedback_parse_polarity("negative"), "negative") == 0);
      assert(strcmp(feedback_parse_polarity("principle"), "principle") == 0);
      assert(feedback_parse_polarity(NULL) == NULL);
      assert(feedback_parse_polarity("") == NULL);
      assert(feedback_parse_polarity("unknown_value") == NULL);
   }

   /* --- feedback_record: create new rule --- */
   {
      sqlite3 *db = db_open(":memory:");
      assert(db != NULL);

      int reinforced = 0;
      int id =
          feedback_record(db, "negative", "do not edit .env", "security risk", -1, &reinforced);
      assert(id > 0);
      assert(reinforced == 0);

      /* Verify rule exists */
      rule_t r;
      int rc = rules_get(db, id, &r);
      assert(rc == 0);
      assert(strcmp(r.polarity, "negative") == 0);
      assert(strstr(r.title, "do not edit .env") != NULL);

      db_stmt_cache_clear();
      db_close(db);
   }

   /* --- feedback_record: reinforce existing rule --- */
   {
      sqlite3 *db = db_open(":memory:");
      assert(db != NULL);

      int reinforced = 0;
      int id1 = feedback_record(db, "negative", "avoid force push", "", -1, &reinforced);
      assert(id1 > 0);
      assert(reinforced == 0);

      /* Record same feedback again */
      int id2 = feedback_record(db, "negative", "avoid force push", "", -1, &reinforced);
      assert(id2 == id1);
      assert(reinforced == 1);

      /* Weight should have increased */
      rule_t r;
      rules_get(db, id1, &r);
      assert(r.weight > 0);

      db_stmt_cache_clear();
      db_close(db);
   }

   /* --- feedback_record: weight override --- */
   {
      sqlite3 *db = db_open(":memory:");
      assert(db != NULL);

      int reinforced = 0;
      int id = feedback_record(db, "positive", "always run tests", "", 75, &reinforced);
      assert(id > 0);

      rule_t r;
      rules_get(db, id, &r);
      assert(r.weight == 75);

      db_stmt_cache_clear();
      db_close(db);
   }

   printf("all tests passed\n");
   return 0;
}
