/* rules.c: rule listing, tier classification, polarity symbols, rules.md generation */
#include "aimee.h"
#include <stdlib.h>

static void row_to_rule(sqlite3_stmt *stmt, rule_t *r)
{
   r->id = sqlite3_column_int(stmt, 0);
   const char *pol = (const char *)sqlite3_column_text(stmt, 1);
   const char *tit = (const char *)sqlite3_column_text(stmt, 2);
   const char *desc = (const char *)sqlite3_column_text(stmt, 3);
   r->weight = sqlite3_column_int(stmt, 4);
   const char *dom = (const char *)sqlite3_column_text(stmt, 5);
   const char *cat = (const char *)sqlite3_column_text(stmt, 6);
   const char *uat = (const char *)sqlite3_column_text(stmt, 7);
   const char *dt = (const char *)sqlite3_column_text(stmt, 8);

   snprintf(r->polarity, sizeof(r->polarity), "%s", pol ? pol : "");
   snprintf(r->title, sizeof(r->title), "%s", tit ? tit : "");
   snprintf(r->description, sizeof(r->description), "%s", desc ? desc : "");
   snprintf(r->domain, sizeof(r->domain), "%s", dom ? dom : "");
   snprintf(r->created_at, sizeof(r->created_at), "%s", cat ? cat : "");
   snprintf(r->updated_at, sizeof(r->updated_at), "%s", uat ? uat : "");
   snprintf(r->directive_type, sizeof(r->directive_type), "%s", dt ? dt : "");
}

int rules_list(sqlite3 *db, rule_t *out, int max_rules)
{
   const char *sql =
       "SELECT id, polarity, title, description, weight, domain,"
       " created_at, updated_at, directive_type FROM rules ORDER BY weight DESC, title ASC";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return 0;

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max_rules)
   {
      row_to_rule(stmt, &out[count]);
      count++;
   }
   sqlite3_reset(stmt);
   return count;
}

int rules_list_by_tier(sqlite3 *db, int min_weight, rule_t *out, int max_rules)
{
   const char *sql = "SELECT id, polarity, title, description, weight, domain,"
                     " created_at, updated_at, directive_type FROM rules"
                     " WHERE weight >= ? ORDER BY weight DESC, title ASC";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return 0;

   sqlite3_bind_int(stmt, 1, min_weight);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max_rules)
   {
      row_to_rule(stmt, &out[count]);
      count++;
   }
   sqlite3_reset(stmt);
   return count;
}

int rules_get(sqlite3 *db, int id, rule_t *out)
{
   const char *sql = "SELECT id, polarity, title, description, weight, domain,"
                     " created_at, updated_at, directive_type FROM rules WHERE id = ?";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return -1;

   sqlite3_bind_int(stmt, 1, id);

   int rc = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      row_to_rule(stmt, out);
      rc = 0;
   }
   sqlite3_reset(stmt);
   return rc;
}

int rules_find_by_title(sqlite3 *db, const char *title, rule_t *out)
{
   const char *sql =
       "SELECT id, polarity, title, description, weight, domain,"
       " created_at, updated_at, directive_type FROM rules WHERE LOWER(title) = LOWER(?)";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return -1;

   sqlite3_bind_text(stmt, 1, title, -1, SQLITE_TRANSIENT);

   int rc = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      row_to_rule(stmt, out);
      rc = 0;
   }
   sqlite3_reset(stmt);
   return rc;
}

int rules_delete(sqlite3 *db, int id)
{
   const char *sql = "DELETE FROM rules WHERE id = ?";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return -1;

   sqlite3_bind_int(stmt, 1, id);
   DB_STEP_LOG(stmt, "rules_delete");
   int changes = sqlite3_changes(db);
   sqlite3_reset(stmt);
   if (changes > 0)
      rules_cache_invalidate();
   return changes > 0 ? 0 : -1;
}

int rules_update_weight(sqlite3 *db, int id, int weight)
{
   char ts[32];
   now_utc(ts, sizeof(ts));

   const char *sql = "UPDATE rules SET weight = ?, updated_at = ? WHERE id = ?";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return -1;

   sqlite3_bind_int(stmt, 1, weight);
   sqlite3_bind_text(stmt, 2, ts, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 3, id);
   DB_STEP_LOG(stmt, "rules_update_weight");
   int changes = sqlite3_changes(db);
   sqlite3_reset(stmt);
   if (changes > 0)
      rules_cache_invalidate();
   return changes > 0 ? 0 : -1;
}

const char *rules_tier(int weight)
{
   if (weight >= 75)
      return "Rule";
   if (weight >= 50)
      return "Inclination";
   return "Archived";
}

char rules_polarity_symbol(const char *polarity)
{
   if (!polarity)
      return '~';
   if (strcmp(polarity, "positive") == 0)
      return '+';
   if (strcmp(polarity, "negative") == 0)
      return '-';
   return '~';
}

static char g_rules_cache_hash[32];
static char *g_rules_cache_output;

void rules_cache_invalidate(void)
{
   g_rules_cache_hash[0] = '\0';
}

static void rules_hash(sqlite3 *db, char *buf, size_t len)
{
   buf[0] = '\0';
   static const char *sql = "SELECT COUNT(*), MAX(updated_at) FROM rules";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      int cnt = sqlite3_column_int(stmt, 0);
      const char *ts = (const char *)sqlite3_column_text(stmt, 1);
      snprintf(buf, len, "%d:%s", cnt, ts ? ts : "");
   }
   sqlite3_reset(stmt);
}

char *rules_generate(sqlite3 *db)
{
   /* Check in-process cache */
   if (!getenv("AIMEE_NO_CACHE") && g_rules_cache_hash[0])
   {
      char cur[32];
      rules_hash(db, cur, sizeof(cur));
      if (strcmp(cur, g_rules_cache_hash) == 0 && g_rules_cache_output)
         return strdup(g_rules_cache_output);
   }

   rule_t rules[128];
   int count = rules_list(db, rules, 128);

   /* Allocate output buffer */
   size_t cap = MAX_SESSION_CHARS + 256;
   char *buf = malloc(cap);
   if (!buf)
      return NULL;

   int pos = snprintf(buf, cap, "# Rules\n\n");
   int emitted = 0;

   /* Emit rules tier first (>=75), then inclinations (>=50), then rest */
   int tiers[] = {75, 50, 0};
   int tier_max[] = {100, 74, 49};

   for (int t = 0; t < 3 && emitted < MAX_SESSION_RULES; t++)
   {
      for (int i = 0; i < count && emitted < MAX_SESSION_RULES; i++)
      {
         int w = rules[i].weight;
         if (w < tiers[t] || w > tier_max[t])
            continue;

         char sym = rules_polarity_symbol(rules[i].polarity);
         const char *text = rules[i].description;
         if (strlen(text) == 0)
            text = rules[i].title;

         /* Truncate rule text */
         char truncated[MAX_RULE_TEXT_LEN + 4];
         if (strlen(text) > MAX_RULE_TEXT_LEN)
         {
            memcpy(truncated, text, MAX_RULE_TEXT_LEN);
            memcpy(truncated + MAX_RULE_TEXT_LEN, "...", 4); /* includes NUL */
            text = truncated;
         }

         /* Prefix hard/soft directive type */
         const char *prefix = "";
         if (rules[i].directive_type[0] == 'h')
            prefix = "MUST: ";
         else if (rules[i].directive_type[0] == 's' && rules[i].directive_type[1] == 'o')
            prefix = "SHOULD: ";

         char line[512];
         int llen = snprintf(line, sizeof(line), "- (%c %d) %s%s\n", sym, w, prefix, text);

         if (pos + llen >= (int)MAX_SESSION_CHARS)
            break;

         memcpy(buf + pos, line, llen);
         pos += llen;
         emitted++;
      }
   }

   buf[pos] = '\0';

   /* Update in-process cache */
   if (!getenv("AIMEE_NO_CACHE"))
   {
      free(g_rules_cache_output);
      g_rules_cache_output = strdup(buf);
      rules_hash(db, g_rules_cache_hash, sizeof(g_rules_cache_hash));
   }

   return buf;
}

/* --- Rule Weight Decay --- */

#define DECAY_INTERVAL_SOFT 14 /* days between decay for soft rules */
#define DECAY_INTERVAL_HARD 42 /* days between decay for hard directives */
#define DECAY_AMOUNT        5  /* weight reduction per interval */
#define ARCHIVE_THRESHOLD   10 /* weight below which rules may be archived */
#define ARCHIVE_GRACE_DAYS  30 /* days at low weight before archival */

int rules_decay(sqlite3 *db)
{
   if (!db)
      return 0;

   int total = 0;

   /* Decay soft rules (non-hard directives) not reinforced for 14+ days */
   static const char *soft_sql = "UPDATE rules SET weight = MAX(weight - ?, 0),"
                                 " updated_at = datetime('now')"
                                 " WHERE (directive_type IS NULL OR directive_type != 'hard')"
                                 " AND last_reinforced_at IS NOT NULL"
                                 " AND last_reinforced_at < datetime('now', ? || ' days')";
   sqlite3_stmt *stmt = db_prepare(db, soft_sql);
   if (stmt)
   {
      char interval[8];
      snprintf(interval, sizeof(interval), "-%d", DECAY_INTERVAL_SOFT);
      sqlite3_bind_int(stmt, 1, DECAY_AMOUNT);
      sqlite3_bind_text(stmt, 2, interval, -1, SQLITE_TRANSIENT);
      DB_STEP_LOG(stmt, "rules_decay");
      total += sqlite3_changes(db);
      sqlite3_reset(stmt);
   }

   /* Decay hard directives at slower rate (42 days) */
   static const char *hard_sql = "UPDATE rules SET weight = MAX(weight - ?, 0),"
                                 " updated_at = datetime('now')"
                                 " WHERE directive_type = 'hard'"
                                 " AND last_reinforced_at IS NOT NULL"
                                 " AND last_reinforced_at < datetime('now', ? || ' days')";
   stmt = db_prepare(db, hard_sql);
   if (stmt)
   {
      char interval[8];
      snprintf(interval, sizeof(interval), "-%d", DECAY_INTERVAL_HARD);
      sqlite3_bind_int(stmt, 1, DECAY_AMOUNT);
      sqlite3_bind_text(stmt, 2, interval, -1, SQLITE_TRANSIENT);
      DB_STEP_LOG(stmt, "rules_decay");
      total += sqlite3_changes(db);
      sqlite3_reset(stmt);
   }

   /* Archive (delete) rules at low weight for 30+ days */
   static const char *archive_sql = "DELETE FROM rules WHERE weight < ?"
                                    " AND updated_at < datetime('now', ? || ' days')";
   stmt = db_prepare(db, archive_sql);
   if (stmt)
   {
      char grace[8];
      snprintf(grace, sizeof(grace), "-%d", ARCHIVE_GRACE_DAYS);
      sqlite3_bind_int(stmt, 1, ARCHIVE_THRESHOLD);
      sqlite3_bind_text(stmt, 2, grace, -1, SQLITE_TRANSIENT);
      DB_STEP_LOG(stmt, "rules_decay");
      int archived = sqlite3_changes(db);
      total += archived;
      sqlite3_reset(stmt);
   }

   if (total > 0)
      rules_cache_invalidate();

   return total;
}
