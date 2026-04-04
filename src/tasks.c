/* tasks.c: task graph with dependencies, decisions, checkpoints, state machine */
#include "aimee.h"
#include "cJSON.h"

/* --- Row helpers --- */

static void row_to_task(sqlite3_stmt *stmt, aimee_task_t *t)
{
   t->id = sqlite3_column_int64(stmt, 0);
   t->parent_id = sqlite3_column_int64(stmt, 1);

   const char *title = (const char *)sqlite3_column_text(stmt, 2);
   const char *state = (const char *)sqlite3_column_text(stmt, 3);

   t->confidence = sqlite3_column_double(stmt, 4);

   const char *sid = (const char *)sqlite3_column_text(stmt, 5);
   const char *cat = (const char *)sqlite3_column_text(stmt, 6);
   const char *uat = (const char *)sqlite3_column_text(stmt, 7);

   snprintf(t->title, sizeof(t->title), "%s", title ? title : "");
   snprintf(t->state, sizeof(t->state), "%s", state ? state : "");
   snprintf(t->session_id, sizeof(t->session_id), "%s", sid ? sid : "");
   snprintf(t->created_at, sizeof(t->created_at), "%s", cat ? cat : "");
   snprintf(t->updated_at, sizeof(t->updated_at), "%s", uat ? uat : "");
}

static void row_to_decision(sqlite3_stmt *stmt, decision_t *d)
{
   d->id = sqlite3_column_int64(stmt, 0);
   d->task_id = sqlite3_column_int64(stmt, 1);

   const char *opts = (const char *)sqlite3_column_text(stmt, 2);
   const char *chosen = (const char *)sqlite3_column_text(stmt, 3);
   const char *rat = (const char *)sqlite3_column_text(stmt, 4);
   const char *assum = (const char *)sqlite3_column_text(stmt, 5);
   const char *out = (const char *)sqlite3_column_text(stmt, 6);
   const char *cat = (const char *)sqlite3_column_text(stmt, 7);

   snprintf(d->options, sizeof(d->options), "%s", opts ? opts : "");
   snprintf(d->chosen, sizeof(d->chosen), "%s", chosen ? chosen : "");
   snprintf(d->rationale, sizeof(d->rationale), "%s", rat ? rat : "");
   snprintf(d->assumptions, sizeof(d->assumptions), "%s", assum ? assum : "");
   snprintf(d->outcome, sizeof(d->outcome), "%s", out ? out : "");
   snprintf(d->created_at, sizeof(d->created_at), "%s", cat ? cat : "");
}

/* --- Task CRUD --- */

int aimee_task_create(sqlite3 *db, const char *title, const char *session_id, int64_t parent_id,
                      aimee_task_t *out)
{
   if (!db || !title)
      return -1;

   char ts[32];
   now_utc(ts, sizeof(ts));

   static const char *sql = "INSERT INTO tasks (parent_id, title, state, confidence,"
                            " session_id, created_at, updated_at)"
                            " VALUES (?, ?, 'todo', 1.0, ?, ?, ?)";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return -1;

   sqlite3_bind_int64(stmt, 1, parent_id);
   sqlite3_bind_text(stmt, 2, title, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, session_id ? session_id : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, ts, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 5, ts, -1, SQLITE_TRANSIENT);

   if (sqlite3_step(stmt) != SQLITE_DONE)
   {
      sqlite3_reset(stmt);
      return -1;
   }

   int64_t new_id = sqlite3_last_insert_rowid(db);
   sqlite3_reset(stmt);

   if (out)
      aimee_task_get(db, new_id, out);

   return 0;
}

int aimee_task_get(sqlite3 *db, int64_t id, aimee_task_t *out)
{
   static const char *sql = "SELECT id, parent_id, title, state, confidence, session_id,"
                            " created_at, updated_at FROM tasks WHERE id = ?";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return -1;

   sqlite3_bind_int64(stmt, 1, id);

   int rc = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      row_to_task(stmt, out);
      rc = 0;
   }
   sqlite3_reset(stmt);
   return rc;
}

int task_update_state(sqlite3 *db, int64_t id, const char *state)
{
   if (!db || !state)
      return -1;

   char ts[32];
   now_utc(ts, sizeof(ts));

   static const char *sql = "UPDATE tasks SET state = ?, updated_at = ? WHERE id = ?";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return -1;

   sqlite3_bind_text(stmt, 1, state, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, ts, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(stmt, 3, id);
   DB_STEP_LOG(stmt, "task_update_state");
   int changes = sqlite3_changes(db);
   sqlite3_reset(stmt);
   return changes > 0 ? 0 : -1;
}

int aimee_task_list(sqlite3 *db, const char *state, const char *session_id, int limit,
                    aimee_task_t *out, int max)
{
   char query[MAX_QUERY_LEN];
   int pos = 0;
   int bind_idx = 0;
   int state_bind = 0, session_bind = 0;

   pos += snprintf(query + pos, sizeof(query) - pos,
                   "SELECT id, parent_id, title, state, confidence, session_id,"
                   " created_at, updated_at FROM tasks WHERE 1=1");

   if (state && state[0])
   {
      pos += snprintf(query + pos, sizeof(query) - pos, " AND state = ?");
      state_bind = ++bind_idx;
   }
   if (session_id && session_id[0])
   {
      pos += snprintf(query + pos, sizeof(query) - pos, " AND session_id = ?");
      session_bind = ++bind_idx;
   }

   pos += snprintf(query + pos, sizeof(query) - pos, " ORDER BY updated_at DESC");

   if (limit > 0)
      snprintf(query + pos, sizeof(query) - pos, " LIMIT %d", limit);

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, query, -1, &stmt, NULL) != SQLITE_OK)
      return 0;

   if (state_bind)
      sqlite3_bind_text(stmt, state_bind, state, -1, SQLITE_TRANSIENT);
   if (session_bind)
      sqlite3_bind_text(stmt, session_bind, session_id, -1, SQLITE_TRANSIENT);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max)
   {
      row_to_task(stmt, &out[count]);
      count++;
   }
   sqlite3_finalize(stmt);
   return count;
}

int aimee_task_delete(sqlite3 *db, int64_t id)
{
   /* Delete edges first */
   static const char *del_edges = "DELETE FROM task_edges"
                                  " WHERE source_id = ? OR target_id = ?";
   sqlite3_stmt *stmt = db_prepare(db, del_edges);
   if (stmt)
   {
      sqlite3_bind_int64(stmt, 1, id);
      sqlite3_bind_int64(stmt, 2, id);
      DB_STEP_LOG(stmt, "aimee_task_delete");
      sqlite3_reset(stmt);
   }

   static const char *sql = "DELETE FROM tasks WHERE id = ?";
   stmt = db_prepare(db, sql);
   if (!stmt)
      return -1;

   sqlite3_bind_int64(stmt, 1, id);
   DB_STEP_LOG(stmt, "aimee_task_delete");
   int changes = sqlite3_changes(db);
   sqlite3_reset(stmt);
   return changes > 0 ? 0 : -1;
}

/* --- Task Success Criteria --- */

int task_set_success_criteria(sqlite3 *db, int64_t id, const char *criteria)
{
   if (!db || !criteria)
      return -1;

   char ts[32];
   now_utc(ts, sizeof(ts));

   static const char *sql = "UPDATE tasks SET success_criteria = ?, updated_at = ? WHERE id = ?";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return -1;

   sqlite3_bind_text(stmt, 1, criteria, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, ts, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(stmt, 3, id);
   DB_STEP_LOG(stmt, "task_set_success_criteria");
   int changes = sqlite3_changes(db);
   sqlite3_reset(stmt);
   return changes > 0 ? 0 : -1;
}

/* Create a subtask and automatically add a parent edge */
int task_create_subtask(sqlite3 *db, int64_t parent_id, const char *title, const char *session_id,
                        aimee_task_t *out)
{
   if (!db || !title || parent_id <= 0)
      return -1;

   int rc = aimee_task_create(db, title, session_id, parent_id, out);
   if (rc != 0)
      return rc;

   /* Add depends_on edge: subtask depends on parent context */
   if (out)
      task_add_edge(db, out->id, parent_id, "subtask_of");

   return 0;
}

/* --- Task Edges --- */

int task_add_edge(sqlite3 *db, int64_t source, int64_t target, const char *relation)
{
   if (!db || !relation)
      return -1;

   static const char *sql = "INSERT INTO task_edges (source_id, target_id, relation)"
                            " VALUES (?, ?, ?)";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return -1;

   sqlite3_bind_int64(stmt, 1, source);
   sqlite3_bind_int64(stmt, 2, target);
   sqlite3_bind_text(stmt, 3, relation, -1, SQLITE_TRANSIENT);

   if (sqlite3_step(stmt) != SQLITE_DONE)
   {
      sqlite3_reset(stmt);
      return -1;
   }
   sqlite3_reset(stmt);
   return 0;
}

int task_get_edges(sqlite3 *db, int64_t task_id, task_edge_t *out, int max)
{
   static const char *sql = "SELECT id, source_id, target_id, relation FROM task_edges"
                            " WHERE source_id = ?"
                            " UNION ALL"
                            " SELECT id, source_id, target_id, relation FROM task_edges"
                            " WHERE target_id = ?"
                            " LIMIT ?";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return 0;

   sqlite3_bind_int64(stmt, 1, task_id);
   sqlite3_bind_int64(stmt, 2, task_id);
   sqlite3_bind_int(stmt, 3, max);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max)
   {
      out[count].id = sqlite3_column_int64(stmt, 0);
      out[count].source_id = sqlite3_column_int64(stmt, 1);
      out[count].target_id = sqlite3_column_int64(stmt, 2);

      const char *rel = (const char *)sqlite3_column_text(stmt, 3);
      snprintf(out[count].relation, sizeof(out[count].relation), "%s", rel ? rel : "");
      count++;
   }
   sqlite3_reset(stmt);
   return count;
}

int aimee_task_get_subtasks(sqlite3 *db, int64_t parent_id, aimee_task_t *out, int max)
{
   static const char *sql = "SELECT id, parent_id, title, state, confidence, session_id,"
                            " created_at, updated_at FROM tasks"
                            " WHERE parent_id = ? ORDER BY created_at ASC";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return 0;

   sqlite3_bind_int64(stmt, 1, parent_id);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max)
   {
      row_to_task(stmt, &out[count]);
      count++;
   }
   sqlite3_reset(stmt);
   return count;
}

/* --- Decisions --- */

int decision_log(sqlite3 *db, const char *options, const char *chosen, const char *rationale,
                 const char *assumptions, int64_t task_id, decision_t *out)
{
   if (!db || !options || !chosen)
      return -1;

   char ts[32];
   now_utc(ts, sizeof(ts));

   static const char *sql = "INSERT INTO decision_log (task_id, options, chosen, rationale,"
                            " assumptions, created_at)"
                            " VALUES (?, ?, ?, ?, ?, ?)";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return -1;

   sqlite3_bind_int64(stmt, 1, task_id);
   sqlite3_bind_text(stmt, 2, options, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, chosen, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, rationale ? rationale : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 5, assumptions ? assumptions : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 6, ts, -1, SQLITE_TRANSIENT);

   if (sqlite3_step(stmt) != SQLITE_DONE)
   {
      sqlite3_reset(stmt);
      return -1;
   }

   int64_t new_id = sqlite3_last_insert_rowid(db);
   sqlite3_reset(stmt);

   if (out)
      decision_get(db, new_id, out);

   return 0;
}

int decision_get(sqlite3 *db, int64_t id, decision_t *out)
{
   static const char *sql = "SELECT id, task_id, options, chosen, rationale, assumptions,"
                            " outcome, created_at FROM decision_log WHERE id = ?";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return -1;

   sqlite3_bind_int64(stmt, 1, id);

   int rc = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      row_to_decision(stmt, out);
      rc = 0;
   }
   sqlite3_reset(stmt);
   return rc;
}

int decision_record_outcome(sqlite3 *db, int64_t id, const char *outcome)
{
   if (!db || !outcome)
      return -1;

   static const char *sql = "UPDATE decision_log SET outcome = ? WHERE id = ?";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return -1;

   sqlite3_bind_text(stmt, 1, outcome, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(stmt, 2, id);
   DB_STEP_LOG(stmt, "decision_record_outcome");
   int changes = sqlite3_changes(db);
   sqlite3_reset(stmt);
   return changes > 0 ? 0 : -1;
}

int decision_list(sqlite3 *db, const char *outcome, int limit, decision_t *out, int max)
{
   char query[MAX_QUERY_LEN];
   int pos = 0;
   int outcome_bind = 0;

   pos += snprintf(query + pos, sizeof(query) - pos,
                   "SELECT id, task_id, options, chosen, rationale, assumptions,"
                   " outcome, created_at FROM decision_log WHERE 1=1");

   if (outcome && outcome[0])
   {
      pos += snprintf(query + pos, sizeof(query) - pos, " AND outcome = ?");
      outcome_bind = 1;
   }

   pos += snprintf(query + pos, sizeof(query) - pos, " ORDER BY created_at DESC");

   if (limit > 0)
      snprintf(query + pos, sizeof(query) - pos, " LIMIT %d", limit);

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, query, -1, &stmt, NULL) != SQLITE_OK)
      return 0;

   if (outcome_bind)
      sqlite3_bind_text(stmt, 1, outcome, -1, SQLITE_TRANSIENT);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max)
   {
      row_to_decision(stmt, &out[count]);
      count++;
   }
   sqlite3_finalize(stmt);
   return count;
}

/* --- Checkpoints --- */

int checkpoint_create(sqlite3 *db, const char *label, const char *session_id, int64_t task_id,
                      checkpoint_t *out)
{
   if (!db || !label)
      return -1;

   /* Build JSON snapshot of active tasks + current facts +
    * recent decisions */
   cJSON *snap = cJSON_CreateObject();
   if (!snap)
      return -1;

   /* Active tasks */
   {
      cJSON *arr = cJSON_AddArrayToObject(snap, "tasks");
      aimee_task_t tasks[32];
      int tc = aimee_task_list(db, TASK_IN_PROGRESS, NULL, 32, tasks, 32);
      for (int i = 0; i < tc; i++)
      {
         cJSON *t = cJSON_CreateObject();
         cJSON_AddNumberToObject(t, "id", (double)tasks[i].id);
         cJSON_AddStringToObject(t, "title", tasks[i].title);
         cJSON_AddStringToObject(t, "state", tasks[i].state);
         cJSON_AddItemToArray(arr, t);
      }
   }

   /* Current facts (L2) */
   {
      cJSON *arr = cJSON_AddArrayToObject(snap, "facts");
      memory_t mems[16];
      int mc = memory_list(db, TIER_L2, KIND_FACT, 16, mems, 16);
      for (int i = 0; i < mc; i++)
      {
         cJSON *m = cJSON_CreateObject();
         cJSON_AddStringToObject(m, "key", mems[i].key);
         cJSON_AddStringToObject(m, "content", mems[i].content);
         cJSON_AddItemToArray(arr, m);
      }
   }

   /* Recent decisions */
   {
      cJSON *arr = cJSON_AddArrayToObject(snap, "decisions");
      decision_t decs[8];
      int dc = decision_list(db, NULL, 8, decs, 8);
      for (int i = 0; i < dc; i++)
      {
         cJSON *d = cJSON_CreateObject();
         cJSON_AddStringToObject(d, "chosen", decs[i].chosen);
         cJSON_AddStringToObject(d, "rationale", decs[i].rationale);
         cJSON_AddItemToArray(arr, d);
      }
   }

   char *snap_str = cJSON_PrintUnformatted(snap);
   cJSON_Delete(snap);
   if (!snap_str)
      return -1;

   char ts[32];
   now_utc(ts, sizeof(ts));

   static const char *sql = "INSERT INTO checkpoints (task_id, session_id, label,"
                            " snapshot, created_at)"
                            " VALUES (?, ?, ?, ?, ?)";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
   {
      free(snap_str);
      return -1;
   }

   sqlite3_bind_int64(stmt, 1, task_id);
   sqlite3_bind_text(stmt, 2, session_id ? session_id : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, label, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, snap_str, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 5, ts, -1, SQLITE_TRANSIENT);

   int rc = -1;
   if (sqlite3_step(stmt) == SQLITE_DONE)
   {
      int64_t new_id = sqlite3_last_insert_rowid(db);
      sqlite3_reset(stmt);

      if (out)
         checkpoint_get(db, new_id, out);
      rc = 0;
   }
   else
   {
      sqlite3_reset(stmt);
   }

   free(snap_str);
   return rc;
}

int checkpoint_get(sqlite3 *db, int64_t id, checkpoint_t *out)
{
   static const char *sql = "SELECT id, task_id, session_id, label, snapshot, created_at"
                            " FROM checkpoints WHERE id = ?";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return -1;

   sqlite3_bind_int64(stmt, 1, id);

   int rc = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      out->id = sqlite3_column_int64(stmt, 0);
      out->task_id = sqlite3_column_int64(stmt, 1);

      const char *sid = (const char *)sqlite3_column_text(stmt, 2);
      const char *lbl = (const char *)sqlite3_column_text(stmt, 3);
      const char *snap = (const char *)sqlite3_column_text(stmt, 4);
      const char *cat = (const char *)sqlite3_column_text(stmt, 5);

      snprintf(out->session_id, sizeof(out->session_id), "%s", sid ? sid : "");
      snprintf(out->label, sizeof(out->label), "%s", lbl ? lbl : "");
      snprintf(out->snapshot, sizeof(out->snapshot), "%s", snap ? snap : "");
      snprintf(out->created_at, sizeof(out->created_at), "%s", cat ? cat : "");
      rc = 0;
   }
   sqlite3_reset(stmt);
   return rc;
}

int checkpoint_list(sqlite3 *db, int limit, checkpoint_t *out, int max)
{
   static const char *sql = "SELECT id, task_id, session_id, label, snapshot, created_at"
                            " FROM checkpoints ORDER BY created_at DESC LIMIT ?";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return 0;

   sqlite3_bind_int(stmt, 1, limit > 0 ? limit : max);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max)
   {
      out[count].id = sqlite3_column_int64(stmt, 0);
      out[count].task_id = sqlite3_column_int64(stmt, 1);

      const char *sid = (const char *)sqlite3_column_text(stmt, 2);
      const char *lbl = (const char *)sqlite3_column_text(stmt, 3);
      const char *snap = (const char *)sqlite3_column_text(stmt, 4);
      const char *cat = (const char *)sqlite3_column_text(stmt, 5);

      snprintf(out[count].session_id, sizeof(out[count].session_id), "%s", sid ? sid : "");
      snprintf(out[count].label, sizeof(out[count].label), "%s", lbl ? lbl : "");
      snprintf(out[count].snapshot, sizeof(out[count].snapshot), "%s", snap ? snap : "");
      snprintf(out[count].created_at, sizeof(out[count].created_at), "%s", cat ? cat : "");
      count++;
   }
   sqlite3_reset(stmt);
   return count;
}

int checkpoint_restore(sqlite3 *db, int64_t id, const char *session_id)
{
   if (!db)
      return -1;

   /* Get the checkpoint */
   checkpoint_t cp;
   if (checkpoint_get(db, id, &cp) != 0)
      return -1;

   /* Inject snapshot as L0 scratch memory */
   char key[128];
   snprintf(key, sizeof(key), "checkpoint_restore:%lld", (long long)id);

   return memory_insert(db, TIER_L0, KIND_SCRATCH, key, cp.snapshot, 1.0, session_id, NULL);
}

int checkpoint_delete(sqlite3 *db, int64_t id)
{
   static const char *sql = "DELETE FROM checkpoints WHERE id = ?";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return -1;

   sqlite3_bind_int64(stmt, 1, id);
   DB_STEP_LOG(stmt, "checkpoint_delete");
   int changes = sqlite3_changes(db);
   sqlite3_reset(stmt);
   return changes > 0 ? 0 : -1;
}

/* --- Active task lookup --- */

int64_t task_get_active(sqlite3 *db, const char *session_id)
{
   if (!db || !session_id)
      return 0;

   static const char *sql = "SELECT id FROM tasks"
                            " WHERE state = 'in_progress' AND session_id = ?"
                            " ORDER BY updated_at DESC LIMIT 1";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return 0;

   sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);

   int64_t result = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      result = sqlite3_column_int64(stmt, 0);

   sqlite3_reset(stmt);
   return result;
}
