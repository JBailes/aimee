#ifndef DEC_AGENT_CONFIG_H
#define DEC_AGENT_CONFIG_H 1

#include "agent_types.h"

int agent_load_config(agent_config_t *cfg);
int agent_save_config(const agent_config_t *cfg);
const char *agent_config_path(void);
agent_t *agent_route(agent_config_t *cfg, const char *role);
agent_t *agent_find(agent_config_t *cfg, const char *name);
int agent_has_role(const agent_t *agent, const char *role);
int agent_is_exec_role(const agent_t *agent, const char *role);
void agent_expand_env(const char *src, char *dst, size_t dst_len);
int agent_resolve_auth(const agent_t *agent, char *buf, size_t buf_len);

#endif /* DEC_AGENT_CONFIG_H */
