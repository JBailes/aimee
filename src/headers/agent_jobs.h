#ifndef DEC_AGENT_JOBS_H
#define DEC_AGENT_JOBS_H 1

#include "agent_types.h"

/* Durable jobs */
int agent_job_create(sqlite3 *db, const char *role, const char *prompt,
                     const char *agent_name);
void agent_job_update(sqlite3 *db, int job_id, const char *status,
                      int turn, const char *response);
void agent_job_heartbeat(sqlite3 *db, int job_id);
void agent_set_durable_job(sqlite3 *db, int job_id);
sqlite3 *agent_get_durable_db(void);
int agent_get_durable_job_id(void);
int agent_job_resume(sqlite3 *db, agent_config_t *cfg, int job_id,
                     agent_result_t *out);

/* Result cache */
char *agent_cache_get(sqlite3 *db, const char *role, const char *prompt);
void agent_cache_put(sqlite3 *db, const char *role, const char *prompt,
                     const agent_result_t *result);

/* One-shot hints */
char *agent_find_hint(sqlite3 *db, const char *role, const char *prompt);

#endif /* DEC_AGENT_JOBS_H */
