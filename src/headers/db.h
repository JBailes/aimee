#ifndef DEC_DB_H
#define DEC_DB_H 1

/* Pragma profile mode: CLI is short-lived (FULL sync), server is long-lived (NORMAL sync). */
typedef enum
{
   DB_MODE_CLI = 0,
   DB_MODE_SERVER
} db_mode_t;

/* Apply the appropriate SQLite pragma profile for the given mode. */
void db_apply_pragmas(sqlite3 *db, db_mode_t mode);

/* Open database with migrations. Returns NULL on failure. */
sqlite3 *db_open(const char *path);

/* Open database without migrations (hot path). Falls back to db_open if new. */
sqlite3 *db_open_fast(const char *path);

/* Close database. */
void db_close(sqlite3 *db);

/* Default database path: ~/.config/aimee/aimee.db */
const char *db_default_path(void);

/* Prepared statement cache: get or create. */
sqlite3_stmt *db_prepare(sqlite3 *db, const char *sql);

/* Close all cached statements. Call before db_close. */
void db_stmt_cache_clear(void);

/* Close cached statements for a specific db connection only. */
void db_stmt_cache_clear_for(sqlite3 *db);

/* Step a statement and log on failure. Use for fire-and-forget writes
 * (provenance, metrics, health) where silent failure causes data loss. */
#define DB_STEP_LOG(stmt, label)                                                                   \
   do                                                                                              \
   {                                                                                               \
      int _rc = sqlite3_step(stmt);                                                                \
      if (_rc != SQLITE_DONE && _rc != SQLITE_ROW)                                                 \
         fprintf(stderr, "aimee: sqlite3_step failed in %s: %s\n", (label),                        \
                 sqlite3_errmsg(sqlite3_db_handle(stmt)));                                         \
   } while (0)

/* Check FTS5 availability. */
int db_fts5_available(sqlite3 *db);

/* Create a manual backup. out_path may be NULL for auto-generated name. */
int db_backup(const char *db_path, const char *out_path);

/* Run integrity check (full=1) or quick_check (full=0). Returns 0 if ok. */
int db_check(const char *db_path, int full);

/* Run PRAGMA quick_check on an already-open database. Returns 0 if ok. */
int db_quick_check(sqlite3 *db);

/* Recover from the most recent valid backup. With force=1, creates a fresh
 * database if no backup exists. Returns 0 on success. */
int db_recover(const char *db_path, int force);

/* Return the next available migration version number. */
int db_next_migration_version(void);

/* Validate migration IDs are unique and strictly increasing.
 * Returns 0 on success, -1 on duplicate/ordering error.
 * If err_buf is not NULL, writes a description of the first error found. */
int db_validate_migrations(char *err_buf, size_t err_len);

#endif /* DEC_DB_H */
