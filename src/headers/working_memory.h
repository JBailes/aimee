#ifndef DEC_WORKING_MEMORY_H
#define DEC_WORKING_MEMORY_H 1

#include <sqlite3.h>

#define WM_MAX_KEY_LEN   256
#define WM_MAX_VALUE_LEN 4096
#define WM_MAX_RESULTS   64
#define WM_DEFAULT_TTL   3600 /* 1 hour */

typedef struct
{
   int64_t id;
   char session_id[128];
   char key[WM_MAX_KEY_LEN];
   char value[WM_MAX_VALUE_LEN];
   char category[64];
   char created_at[32];
   char updated_at[32];
   char expires_at[32];
} wm_entry_t;

/* Set a key-value pair. Updates if key exists, inserts otherwise.
 * ttl_seconds: 0 means no expiry, >0 sets expires_at. */
int wm_set(sqlite3 *db, const char *session_id, const char *key, const char *value,
           const char *category, int ttl_seconds);

/* Get a value by key. Returns 0 on success, -1 if not found/expired. */
int wm_get(sqlite3 *db, const char *session_id, const char *key, wm_entry_t *out);

/* List all entries for a session. Returns count. */
int wm_list(sqlite3 *db, const char *session_id, const char *category, wm_entry_t *out, int max);

/* Delete a specific key. */
int wm_delete(sqlite3 *db, const char *session_id, const char *key);

/* Clear all working memory for a session. */
int wm_clear(sqlite3 *db, const char *session_id);

/* Remove expired entries across all sessions. Returns count removed. */
int wm_gc(sqlite3 *db);

/* Assemble working memory into a context string for the agent.
 * Returns malloc'd string (caller frees). */
char *wm_assemble_context(sqlite3 *db, const char *session_id);

#endif
