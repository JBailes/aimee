/* feedback.c: feedback recording, polarity parsing, rule reinforcement */
#include "aimee.h"
#include "rules.h"

const char *feedback_parse_polarity(const char *input)
{
   if (!input || !*input)
      return NULL;

   if (strcmp(input, "+") == 0 || strcmp(input, "posi") == 0 || strcmp(input, "positive") == 0)
      return "positive";

   if (strcmp(input, "-") == 0 || strcmp(input, "negi") == 0 || strcmp(input, "negative") == 0)
      return "negative";

   if (strcmp(input, "principle") == 0)
      return "principle";

   return NULL;
}

int feedback_record(sqlite3 *db, const char *polarity, const char *title, const char *description,
                    int weight_override, int *reinforced)
{
   if (!polarity || !title)
      return -1;

   *reinforced = 0;

   /* Check for existing rule with same title */
   rule_t existing;
   if (rules_find_by_title(db, title, &existing) == 0)
   {
      /* Reinforce: bump weight by 50, cap at 100 */
      int new_weight = existing.weight + 50;
      if (new_weight > 100)
         new_weight = 100;

      char ts[32];
      now_utc(ts, sizeof(ts));

      const char *sql = "UPDATE rules SET weight = ?, description = ?,"
                        " updated_at = ?, last_reinforced_at = ? WHERE id = ?";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (!stmt)
         return -1;

      sqlite3_bind_int(stmt, 1, new_weight);
      sqlite3_bind_text(stmt, 2, description ? description : existing.description, -1,
                        SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 3, ts, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 4, ts, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(stmt, 5, existing.id);
      DB_STEP_LOG(stmt, "feedback_record");
      sqlite3_reset(stmt);

      *reinforced = 1;
      rules_cache_invalidate();
      return existing.id;
   }

   /* Insert new rule */
   int weight = 50;
   if (weight_override >= 0)
      weight = weight_override;
   if (weight > 100)
      weight = 100;

   char ts[32];
   now_utc(ts, sizeof(ts));

   const char *sql = "INSERT INTO rules (polarity, title, description, weight,"
                     " domain, created_at, updated_at)"
                     " VALUES (?, ?, ?, ?, '', ?, ?)";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return -1;

   sqlite3_bind_text(stmt, 1, polarity, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, title, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, description ? description : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 4, weight);
   sqlite3_bind_text(stmt, 5, ts, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 6, ts, -1, SQLITE_TRANSIENT);

   if (sqlite3_step(stmt) != SQLITE_DONE)
   {
      sqlite3_reset(stmt);
      return -1;
   }

   int id = (int)sqlite3_last_insert_rowid(db);
   sqlite3_reset(stmt);
   rules_cache_invalidate();
   return id;
}
