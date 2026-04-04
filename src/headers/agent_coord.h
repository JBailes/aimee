#ifndef DEC_AGENT_COORD_H
#define DEC_AGENT_COORD_H 1

#include "agent_types.h"

int agent_coordinate(sqlite3 *db, agent_config_t *cfg,
                     const char *task, agent_result_t *out);
int agent_vote(sqlite3 *db, agent_config_t *cfg, const char *role,
               const char *prompt, int n_voters, agent_result_t *out);
int directive_check_tool(sqlite3 *db, const char *tool_name, const char *args_json,
                         char *reason_out, size_t reason_len);
void directive_expire_session(sqlite3 *db);

/* Delegation checkpoint: save/load failure context for retry */
int delegation_checkpoint_save(sqlite3 *db, const char *delegation_id, const char *job_id,
                               int attempt, const char *steps_json, const char *last_output,
                               const char *error);
int delegation_checkpoint_load(sqlite3 *db, const char *delegation_id,
                               char *steps_out, size_t steps_cap,
                               char *error_out, size_t error_cap,
                               char *output_out, size_t output_cap);

#endif /* DEC_AGENT_COORD_H */
