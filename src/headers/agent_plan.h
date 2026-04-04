#ifndef DEC_AGENT_PLAN_H
#define DEC_AGENT_PLAN_H 1

#include "agent_types.h"

typedef struct
{
   int id;
   char action[128];
   char precondition[512];
   char success_predicate[512];
   char rollback[512];
   int depends_on[AGENT_MAX_PLAN_DEPS];
   int dep_count;
   int status; /* 0=pending, 1=running, 2=done, 3=failed, 4=rolled_back */
   char output[4096];
} plan_step_t;

typedef struct
{
   int id;
   char agent_name[MAX_AGENT_NAME];
   char task[1024];
   char status[16];
   plan_step_t steps[AGENT_MAX_PLAN_STEPS];
   int step_count;
} plan_t;

int agent_plan_create(sqlite3 *db, const char *agent_name, const char *task,
                      const struct cJSON *steps_json);
int agent_plan_load(sqlite3 *db, int plan_id, plan_t *out);
int agent_plan_execute(sqlite3 *db, plan_t *plan, const agent_t *agent, int timeout_ms);
int agent_plan_rollback_step(sqlite3 *db, plan_t *plan, int step_idx, int timeout_ms);
int agent_plan_list(sqlite3 *db, plan_t *out, int max);
int agent_execute_with_plan(sqlite3 *db, const agent_t *agent,
                            const agent_network_t *network,
                            const char *system_prompt,
                            const char *user_prompt,
                            int max_tokens, double temperature,
                            agent_result_t *out);

#endif /* DEC_AGENT_PLAN_H */
