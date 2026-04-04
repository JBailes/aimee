/* working_memory.c: session-scoped key-value scratch space with TTL */
#include "aimee.h"
#include <time.h>

/* --- Row helper --- */

static void row_to_wm(sqlite3_stmt *stmt, wm_entry_t *e)
{
   e->id = sqlite3_column_int64(stmt, 0);
   const char *sid = (const char *)sqlite3_column_text(stmt, 1);
   const char *key = (const char *)sqlite3_column_text(stmt, 2);
   const char *val = (const char *)sqlite3_column_text(stmt, 3);
   const char *cat = (const char *)sqlite3_column_text(stmt, 4);
   const char *ca = (const char *)sqlite3_column_text(stmt, 5);
   const char *ua = (const char *)sqlite3_column_text(stmt, 6);
   const char *ea = (const char *)sqlite3_column_text(stmt, 7);

   snprintf(e->session_id, sizeof(e->session_id), "%s", sid ? sid : "");
   snprintf(e->key, sizeof(e->key), "%s", key ? key : "");
   snprintf(e->value, sizeof(e->value), "%s", val ? val : "");
   snprintf(e->category, sizeof(e->category), "%s", cat ? cat : "general");
   snprintf(e->created_at, sizeof(e->created_at), "%s", ca ? ca : "");
   snprintf(e->updated_at, sizeof(e->updated_at), "%s", ua ? ua : "");
   snprintf(e->expires_at, sizeof(e->expires_at), "%s", ea ? ea : "");
}

/* Compute an expiry timestamp ttl_seconds from now. */
static void compute_expires(int ttl_seconds, char *buf, size_t len)
{
   if (ttl_seconds <= 0)
   {
      buf[0] = '\0';
      return;
   }
   time_t t = time(NULL) + ttl_seconds;
   struct tm tm_buf;
   gmtime_r(&t, &tm_buf);
   strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
}

/* --- Public API --- */

int wm_set(sqlite3 *db, const char *session_id, const char *key, const char *value,
           const char *category, int ttl_seconds)
{
   if (!db || !session_id || !key || !value)
      return -1;

   static const char *sql =
       "INSERT OR REPLACE INTO working_memory"
       " (session_id, key, value, category, created_at, updated_at, expires_at)"
       " VALUES (?, ?, ?, ?, ?, ?, ?)";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return -1;

   char ts[32];
   now_utc(ts, sizeof(ts));

   char expires[32];
   compute_expires(ttl_seconds, expires, sizeof(expires));

   if (!category || !category[0])
      category = "general";

   sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, key, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, value, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, category, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 5, ts, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 6, ts, -1, SQLITE_TRANSIENT);
   if (expires[0])
      sqlite3_bind_text(stmt, 7, expires, -1, SQLITE_TRANSIENT);
   else
      sqlite3_bind_null(stmt, 7);

   int rc = sqlite3_step(stmt);
   sqlite3_reset(stmt);
   return (rc == SQLITE_DONE) ? 0 : -1;
}

int wm_get(sqlite3 *db, const char *session_id, const char *key, wm_entry_t *out)
{
   if (!db || !session_id || !key || !out)
      return -1;

   static const char *sql =
       "SELECT id, session_id, key, value, category, created_at, updated_at, expires_at"
       " FROM working_memory"
       " WHERE session_id = ? AND key = ? AND (expires_at IS NULL OR expires_at > ?)";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return -1;

   char ts[32];
   now_utc(ts, sizeof(ts));

   sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, key, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, ts, -1, SQLITE_TRANSIENT);

   int rc = sqlite3_step(stmt);
   if (rc != SQLITE_ROW)
   {
      sqlite3_reset(stmt);
      return -1;
   }

   row_to_wm(stmt, out);
   sqlite3_reset(stmt);
   return 0;
}

int wm_list(sqlite3 *db, const char *session_id, const char *category, wm_entry_t *out, int max)
{
   if (!db || !session_id || !out || max <= 0)
      return 0;

   const char *sql_all =
       "SELECT id, session_id, key, value, category, created_at, updated_at, expires_at"
       " FROM working_memory"
       " WHERE session_id = ? AND (expires_at IS NULL OR expires_at > ?)"
       " ORDER BY updated_at DESC";

   const char *sql_cat =
       "SELECT id, session_id, key, value, category, created_at, updated_at, expires_at"
       " FROM working_memory"
       " WHERE session_id = ? AND (expires_at IS NULL OR expires_at > ?) AND category = ?"
       " ORDER BY updated_at DESC";

   char ts[32];
   now_utc(ts, sizeof(ts));

   sqlite3_stmt *stmt;
   if (category && category[0])
   {
      stmt = db_prepare(db, sql_cat);
      if (!stmt)
         return 0;
      sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 2, ts, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 3, category, -1, SQLITE_TRANSIENT);
   }
   else
   {
      stmt = db_prepare(db, sql_all);
      if (!stmt)
         return 0;
      sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 2, ts, -1, SQLITE_TRANSIENT);
   }

   int count = 0;
   while (count < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      row_to_wm(stmt, &out[count]);
      count++;
   }
   sqlite3_reset(stmt);
   return count;
}

int wm_delete(sqlite3 *db, const char *session_id, const char *key)
{
   if (!db || !session_id || !key)
      return -1;

   static const char *sql = "DELETE FROM working_memory WHERE session_id = ? AND key = ?";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return -1;

   sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, key, -1, SQLITE_TRANSIENT);

   int rc = sqlite3_step(stmt);
   sqlite3_reset(stmt);
   return (rc == SQLITE_DONE) ? 0 : -1;
}

int wm_clear(sqlite3 *db, const char *session_id)
{
   if (!db || !session_id)
      return -1;

   static const char *sql = "DELETE FROM working_memory WHERE session_id = ?";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return -1;

   sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);

   int rc = sqlite3_step(stmt);
   sqlite3_reset(stmt);
   return (rc == SQLITE_DONE) ? 0 : -1;
}

int wm_gc(sqlite3 *db)
{
   if (!db)
      return 0;

   char ts[32];
   now_utc(ts, sizeof(ts));

   /* Count expired entries first */
   static const char *count_sql =
       "SELECT COUNT(*) FROM working_memory WHERE expires_at IS NOT NULL AND expires_at <= ?";
   sqlite3_stmt *cs = db_prepare(db, count_sql);
   if (!cs)
      return 0;
   sqlite3_bind_text(cs, 1, ts, -1, SQLITE_TRANSIENT);
   int removed = 0;
   if (sqlite3_step(cs) == SQLITE_ROW)
      removed = sqlite3_column_int(cs, 0);
   sqlite3_reset(cs);

   if (removed == 0)
      return 0;

   /* Delete expired */
   static const char *del_sql =
       "DELETE FROM working_memory WHERE expires_at IS NOT NULL AND expires_at <= ?";
   sqlite3_stmt *ds = db_prepare(db, del_sql);
   if (!ds)
      return 0;
   sqlite3_bind_text(ds, 1, ts, -1, SQLITE_TRANSIENT);
   DB_STEP_LOG(ds, "wm_gc");
   sqlite3_reset(ds);
   return removed;
}

char *wm_assemble_context(sqlite3 *db, const char *session_id)
{
   if (!db || !session_id)
      return NULL;

   wm_entry_t entries[WM_MAX_RESULTS];
   int count = wm_list(db, session_id, NULL, entries, WM_MAX_RESULTS);
   if (count == 0)
      return NULL;

   /* Estimate buffer size */
   size_t buf_size = 64; /* header */
   for (int i = 0; i < count; i++)
      buf_size +=
          strlen(entries[i].category) + strlen(entries[i].key) + strlen(entries[i].value) + 16;

   char *buf = malloc(buf_size);
   if (!buf)
      return NULL;

   int pos = snprintf(buf, buf_size, "## Working Memory\n");
   for (int i = 0; i < count; i++)
   {
      pos += snprintf(buf + pos, buf_size - (size_t)pos, "[%s] %s: %s\n", entries[i].category,
                      entries[i].key, entries[i].value);
   }

   return buf;
}
