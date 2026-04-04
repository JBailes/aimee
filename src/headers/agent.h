#ifndef DEC_AGENT_H
#define DEC_AGENT_H 1

/*
 * Umbrella header: includes all agent subsystem headers.
 * Prefer including the specific narrow header you need:
 *   agent_types.h  - shared types and constants
 *   agent_config.h - config loading, routing, auth
 *   agent_exec.h   - execution, context, SSH, policy, metrics
 *   agent_plan.h   - plan IR, two-phase execution
 *   agent_eval.h   - eval harness
 *   agent_coord.h  - multi-agent coordination, directives
 *   agent_jobs.h   - durable jobs, cache, hints
 *   agent_tools.h  - tool execution, checkpoints
 */

#include "agent_types.h"
#include "agent_config.h"
#include "agent_exec.h"
#include "agent_plan.h"
#include "agent_eval.h"
#include "agent_coord.h"
#include "agent_jobs.h"
#include "agent_tools.h"

#endif /* DEC_AGENT_H */
