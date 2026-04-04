/* agent_jobs.c: durable jobs, result cache, one-shot hints, durable job context */
#include "aimee.h"
#include "agent_jobs.h"
#include "agent_config.h"
#include "agent_exec.h"
#include "cJSON.h"
#include <unistd.h>

/* --- Durable job context globals --- */

static sqlite3 *g_durable_db = NULL;
static int g_durable_job_id = 0;

void agent_set_durable_job(sqlite3 *db, int job_id)
{
   g_durable_db = db;
   g_durable_job_id = job_id;
}

sqlite3 *agent_get_durable_db(void)
{
   return g_durable_db;
}

int agent_get_durable_job_id(void)
{
   return g_durable_job_id;
}

/* --- Durable jobs --- */

int agent_job_create(sqlite3 *db, const char *role, const char *prompt, const char *agent_name)
{
   if (!db)
      return -1;

   static const char *sql = "INSERT INTO agent_jobs (role, prompt, agent_name, status,"
                            " lease_owner, heartbeat_at, created_at, updated_at)"
                            " VALUES (?, ?, ?, 'running', ?, datetime('now'),"
                            " datetime('now'), datetime('now'))";

   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return -1;

   char pid[32];
   snprintf(pid, sizeof(pid), "%d", (int)getpid());

   sqlite3_reset(stmt);
   sqlite3_bind_text(stmt, 1, role, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, prompt, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, agent_name ? agent_name : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, pid, -1, SQLITE_TRANSIENT);

   if (sqlite3_step(stmt) != SQLITE_DONE)
      return -1;

   return (int)sqlite3_last_insert_rowid(db);
}

void agent_job_update(sqlite3 *db, int job_id, const char *status, int turn, const char *response)
{
   if (!db || job_id <= 0)
      return;

   static const char *sql = "UPDATE agent_jobs SET status = ?, cursor = ?,"
                            " result = ?, updated_at = datetime('now')"
                            " WHERE id = ?";

   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return;

   char cursor[32];
   snprintf(cursor, sizeof(cursor), "%d", turn);

   sqlite3_reset(stmt);
   sqlite3_bind_text(stmt, 1, status, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, cursor, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, response, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 4, job_id);
   DB_STEP_LOG(stmt, "agent_job_update");
}

void agent_job_heartbeat(sqlite3 *db, int job_id)
{
   if (!db || job_id <= 0)
      return;

   static const char *sql = "UPDATE agent_jobs SET heartbeat_at = datetime('now'),"
                            " updated_at = datetime('now') WHERE id = ?";

   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return;

   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, job_id);
   DB_STEP_LOG(stmt, "agent_job_heartbeat");
}

int agent_job_resume(sqlite3 *db, agent_config_t *cfg, int job_id, agent_result_t *out)
{
   if (!db || !cfg || !out || job_id <= 0)
      return -1;

   memset(out, 0, sizeof(*out));

   /* Load job details */
   static const char *job_sql = "SELECT role, prompt, agent_name, status, cursor,"
                                " heartbeat_at FROM agent_jobs WHERE id = ?";
   sqlite3_stmt *stmt = db_prepare(db, job_sql);
   if (!stmt)
      return -1;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, job_id);
   if (sqlite3_step(stmt) != SQLITE_ROW)
   {
      snprintf(out->error, sizeof(out->error), "job %d not found", job_id);
      return -1;
   }

   const char *role = (const char *)sqlite3_column_text(stmt, 0);
   const char *prompt = (const char *)sqlite3_column_text(stmt, 1);
   const char *agent_name = (const char *)sqlite3_column_text(stmt, 2);
   const char *status = (const char *)sqlite3_column_text(stmt, 3);
   const char *cursor_str = (const char *)sqlite3_column_text(stmt, 4);
   const char *heartbeat = (const char *)sqlite3_column_text(stmt, 5);

   /* Only resume stale or failed jobs */
   if (status && strcmp(status, "done") == 0)
   {
      snprintf(out->error, sizeof(out->error), "job %d already completed", job_id);
      return -1;
   }
   if (status && strcmp(status, "cancelled") == 0)
   {
      snprintf(out->error, sizeof(out->error), "job %d was cancelled", job_id);
      return -1;
   }

   /* Copy values before the statement gets reused */
   char j_role[32], j_prompt[4096], j_agent[MAX_AGENT_NAME];
   snprintf(j_role, sizeof(j_role), "%s", role ? role : "execute");
   snprintf(j_prompt, sizeof(j_prompt), "%s", prompt ? prompt : "");
   snprintf(j_agent, sizeof(j_agent), "%s", agent_name ? agent_name : "");
   (void)heartbeat;
   (void)cursor_str;

   /* Claim the job */
   char pid[32];
   snprintf(pid, sizeof(pid), "%d", (int)getpid());
   {
      static const char *claim_sql = "UPDATE agent_jobs SET status = 'running',"
                                     " lease_owner = ?, heartbeat_at = datetime('now')"
                                     " WHERE id = ?";
      sqlite3_stmt *cs = db_prepare(db, claim_sql);
      if (cs)
      {
         sqlite3_reset(cs);
         sqlite3_bind_text(cs, 1, pid, -1, SQLITE_TRANSIENT);
         sqlite3_bind_int(cs, 2, job_id);
         DB_STEP_LOG(cs, "agent_job_resume");
      }
   }

   /* Re-run from scratch (simple strategy: just re-execute the original prompt).
    * The trace from the previous run is preserved for debugging.
    * A more sophisticated approach would replay the conversation from trace,
    * but that requires reconstructing the full message array. */
   agent_t *ag = agent_find(cfg, j_agent);
   if (!ag)
      ag = agent_route(cfg, j_role);
   if (!ag)
   {
      snprintf(out->error, sizeof(out->error), "no agent available for role '%s'", j_role);
      agent_job_update(db, job_id, "failed", 0, out->error);
      return -1;
   }

   int use_tools = ag->tools_enabled && agent_is_exec_role(ag, j_role);
   int rc;
   if (use_tools)
      rc = agent_execute_with_tools(db, ag, &cfg->network, NULL, j_prompt, 0, 0.3, out);
   else
      rc = agent_execute(db, ag, NULL, j_prompt, 0, 0.3, out);

   /* Update job with result */
   agent_job_update(db, job_id, rc == 0 ? "done" : "failed", out->turns,
                    out->response ? out->response : out->error);

   return rc;
}

/* --- Result cache (#2) --- */

static unsigned long hash_string(const char *s)
{
   unsigned long h = 5381;
   int c;
   while ((c = *s++))
      h = ((h << 5) + h) + (unsigned long)c;
   return h;
}

char *agent_cache_get(sqlite3 *db, const char *role, const char *prompt)
{
   if (!db || !role || !prompt)
      return NULL;

   char hash[32];
   snprintf(hash, sizeof(hash), "%lu", hash_string(prompt));

   static const char *sql = "SELECT response FROM agent_cache"
                            " WHERE role = ? AND prompt_hash = ?"
                            " AND created_at > datetime('now', '-' || ? || ' seconds')"
                            " LIMIT 1";

   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return NULL;

   sqlite3_reset(stmt);
   sqlite3_bind_text(stmt, 1, role, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, hash, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 3, AGENT_CACHE_TTL_SECONDS);

   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const char *resp = (const char *)sqlite3_column_text(stmt, 0);
      return resp ? strdup(resp) : NULL;
   }
   return NULL;
}

void agent_cache_put(sqlite3 *db, const char *role, const char *prompt,
                     const agent_result_t *result)
{
   if (!db || !role || !prompt || !result->response || !result->success)
      return;

   char hash[32];
   snprintf(hash, sizeof(hash), "%lu", hash_string(prompt));

   static const char *sql = "INSERT OR REPLACE INTO agent_cache (role, prompt_hash, response,"
                            " confidence, turns, tool_calls, created_at)"
                            " VALUES (?, ?, ?, ?, ?, ?, datetime('now'))";

   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return;

   sqlite3_reset(stmt);
   sqlite3_bind_text(stmt, 1, role, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, hash, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, result->response, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 4, result->confidence);
   sqlite3_bind_int(stmt, 5, result->turns);
   sqlite3_bind_int(stmt, 6, result->tool_calls);
   DB_STEP_LOG(stmt, "agent_cache_put");
}

/* --- One-shot hints (#5) --- */

char *agent_find_hint(sqlite3 *db, const char *role, const char *prompt)
{
   if (!db || !role || !prompt)
      return NULL;

   /* Search past successful feedback episodes for matching role */
   static const char *sql = "SELECT content FROM memories"
                            " WHERE tier IN ('L1', 'L2') AND kind = 'episode'"
                            " AND key LIKE '%' || ? || '%success%'"
                            " ORDER BY last_used_at DESC LIMIT 1";

   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return NULL;

   sqlite3_reset(stmt);
   sqlite3_bind_text(stmt, 1, role, -1, SQLITE_TRANSIENT);

   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const char *content = (const char *)sqlite3_column_text(stmt, 0);
      if (content && content[0])
      {
         size_t hint_len = strlen(content) + 64;
         char *hint = malloc(hint_len);
         if (hint)
         {
            snprintf(hint, hint_len, "Hint from past success: %s", content);
            return hint;
         }
      }
   }
   return NULL;
}
