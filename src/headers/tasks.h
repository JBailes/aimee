#ifndef DEC_TASKS_H
#define DEC_TASKS_H 1

typedef struct
{
   int64_t id;
   int64_t parent_id; /* 0 if no parent */
   char title[256];
   char state[16];
   double confidence;
   char created_at[32];
   char updated_at[32];
   char session_id[128];
} aimee_task_t;

typedef struct
{
   int64_t id;
   int64_t source_id;
   int64_t target_id;
   char relation[32];
} task_edge_t;

typedef struct
{
   int64_t id;
   int64_t task_id; /* 0 if no task */
   char options[1024];
   char chosen[256];
   char rationale[1024];
   char assumptions[512];
   char outcome[32];
   char created_at[32];
} decision_t;

typedef struct
{
   int64_t id;
   int64_t task_id;
   char session_id[128];
   char label[256];
   char snapshot[8192];
   char created_at[32];
} checkpoint_t;

/* Task CRUD */
int aimee_task_create(sqlite3 *db, const char *title, const char *session_id,
                int64_t parent_id, aimee_task_t *out);
int aimee_task_get(sqlite3 *db, int64_t id, aimee_task_t *out);
int task_update_state(sqlite3 *db, int64_t id, const char *state);
int aimee_task_list(sqlite3 *db, const char *state, const char *session_id,
              int limit, aimee_task_t *out, int max);
int aimee_task_delete(sqlite3 *db, int64_t id);

/* Task success criteria */
int task_set_success_criteria(sqlite3 *db, int64_t id, const char *criteria);
int task_create_subtask(sqlite3 *db, int64_t parent_id, const char *title,
                        const char *session_id, aimee_task_t *out);

/* Task edges */
int task_add_edge(sqlite3 *db, int64_t source, int64_t target, const char *relation);
int task_get_edges(sqlite3 *db, int64_t task_id, task_edge_t *out, int max);
int aimee_task_get_subtasks(sqlite3 *db, int64_t parent_id, aimee_task_t *out, int max);

/* Decisions */
int decision_log(sqlite3 *db, const char *options, const char *chosen,
                 const char *rationale, const char *assumptions,
                 int64_t task_id, decision_t *out);
int decision_get(sqlite3 *db, int64_t id, decision_t *out);
int decision_record_outcome(sqlite3 *db, int64_t id, const char *outcome);
int decision_list(sqlite3 *db, const char *outcome, int limit, decision_t *out, int max);

/* Checkpoints */
int checkpoint_create(sqlite3 *db, const char *label, const char *session_id,
                      int64_t task_id, checkpoint_t *out);
int checkpoint_get(sqlite3 *db, int64_t id, checkpoint_t *out);
int checkpoint_list(sqlite3 *db, int limit, checkpoint_t *out, int max);
int checkpoint_restore(sqlite3 *db, int64_t id, const char *session_id);
int checkpoint_delete(sqlite3 *db, int64_t id);

/* Get active in_progress task for a session. Returns task_id or 0. */
int64_t task_get_active(sqlite3 *db, const char *session_id);

#endif /* DEC_TASKS_H */
