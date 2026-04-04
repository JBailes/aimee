# Agent Platform Features

## Problem

Aimee now has executable agents with tool-use loops, context injection,
ephemeral SSH, and background execution. These capabilities work, but
they lack the safety, observability, and coordination infrastructure
needed for reliable autonomous operation. The agent can act, but cannot
reason about confidence, roll back mistakes, coordinate with other
agents, or be audited after the fact.

## Goal

Add ten platform capabilities that make aimee's agent system
production-grade: safe, observable, auditable, and self-improving.

## Features

### 1. Tool Registry with JSON Schema Validation

Every tool gets a formal registration with:
- Input schema (JSON Schema, validated before execution)
- Output schema (validated after execution)
- Side-effect level: `read`, `write`, `destructive`
- Idempotency flag

**Implementation:**

New `tool_registry_t` struct with schema, side_effect_level, idempotent
fields. `dispatch_tool_call()` validates arguments against the schema
before execution. Invalid arguments return an error to the LLM instead
of calling the tool, allowing the LLM to self-correct.

Store tool definitions in a new `tool_registry` table so they persist
and can be extended at runtime:

```sql
CREATE TABLE tool_registry (
   name TEXT PRIMARY KEY,
   description TEXT NOT NULL,
   input_schema TEXT NOT NULL,     -- JSON Schema
   output_schema TEXT,             -- JSON Schema (optional)
   side_effect TEXT NOT NULL DEFAULT 'read',  -- read, write, destructive
   idempotent INTEGER NOT NULL DEFAULT 0,
   enabled INTEGER NOT NULL DEFAULT 1
);
```

The four built-in tools (bash, read_file, write_file, list_files) are
seeded on first run. `bash` is `destructive`, `read_file` and
`list_files` are `read`, `write_file` is `write`.

Validation uses a lightweight JSON Schema subset (type checking, required
fields, enum validation) implemented in C. Full JSON Schema is overkill;
we only need enough to catch missing/wrong-type arguments.

**Affected files:**
- `src/headers/agent.h`: `tool_registry_t` struct, `tool_validate()` prototype
- `src/agent.c`: `tool_validate()`, update `dispatch_tool_call()` to validate first
- `src/db.c`: migration_014 for `tool_registry` table
- `src/tests/test_agent.c`: validation tests (missing field, wrong type, valid input)

### 2. Plan IR (Intermediate Representation)

Persist execution plans as machine-readable DAGs before running them.
Each plan has steps with preconditions, expected artifacts, success
predicates, and rollback strategies.

**Implementation:**

New `plan_step_t` struct and `plan_t` struct:

```c
typedef struct {
   int id;
   char action[128];          /* e.g., "ssh deploy curl health" */
   char precondition[512];    /* e.g., "deploy reachable" */
   char success_predicate[512]; /* e.g., "exit_code == 0" */
   char rollback[512];        /* e.g., "none" or "ssh deploy restart service" */
   int depends_on[8];         /* step IDs this depends on */
   int dep_count;
   int status;                /* 0=pending, 1=running, 2=done, 3=failed, 4=rolled_back */
} plan_step_t;
```

New `execution_plans` and `plan_steps` tables:

```sql
CREATE TABLE execution_plans (
   id INTEGER PRIMARY KEY,
   agent_name TEXT NOT NULL,
   task TEXT NOT NULL,
   status TEXT NOT NULL DEFAULT 'pending', -- pending, running, done, failed
   created_at TEXT NOT NULL DEFAULT (datetime('now'))
);

CREATE TABLE plan_steps (
   id INTEGER PRIMARY KEY,
   plan_id INTEGER NOT NULL REFERENCES execution_plans(id),
   seq INTEGER NOT NULL,
   action TEXT NOT NULL,
   precondition TEXT,
   success_predicate TEXT,
   rollback TEXT,
   status TEXT NOT NULL DEFAULT 'pending',
   output TEXT,
   started_at TEXT,
   finished_at TEXT
);
```

The agentic loop gets a two-phase mode: first ask the LLM to output a
plan (JSON array of steps), then execute each step. If a step fails,
check for a rollback strategy and execute it.

New CLI: `aimee plan list`, `aimee plan show <id>`, `aimee plan replay <id>`.

**Affected files:**
- `src/headers/agent.h`: `plan_step_t`, `plan_t`, plan function prototypes
- `src/agent.c`: `agent_create_plan()`, `agent_execute_plan()`, `agent_rollback_step()`
- `src/db.c`: migration_015 for plan tables
- `src/main.c`: `aimee plan` subcommand

### 3. Execution Transactions with Rollback Checkpoints

Extend checkpointing so mutating steps run in transactional bundles.
Before each mutating tool call, capture pre-state. On failure, auto-rollback.

**Implementation:**

For file writes: save the original file content before overwriting.
For bash commands marked `destructive`: no auto-rollback (log only).
For bash commands marked `write`: capture relevant state first.

New `exec_checkpoint_t`:

```c
typedef struct {
   int step_id;
   char path[MAX_PATH_LEN];    /* file path (for file ops) */
   char *original_content;      /* pre-state (malloc'd) */
   char command[4096];           /* the command that was run */
   int rolled_back;
} exec_checkpoint_t;
```

`dispatch_tool_call()` creates a checkpoint before `write_file` calls
(saves original content). On plan step failure, `agent_rollback_step()`
restores the file from the checkpoint.

For bash commands, rollback is defined in the plan step's `rollback`
field (a shell command to run). If empty, the step is logged as
non-rollbackable.

**Affected files:**
- `src/headers/agent.h`: `exec_checkpoint_t`
- `src/agent.c`: checkpoint creation in `dispatch_tool_call()`, rollback logic

### 4. Policy-as-Code for Guardrails

Support project-local policy files (`.aimee-policy.json`) that define:
- Allowed tools by directory
- Forbidden command patterns (regex)
- Required approval level by risk

**Implementation:**

Policy file format:

```json
{
   "tool_rules": [
      {"path_prefix": "/opt/deploy/", "allowed_tools": ["bash", "read_file", "list_files"]},
      {"path_prefix": "/etc/", "allowed_tools": ["read_file"]}
   ],
   "forbidden_commands": [
      "rm -rf /",
      "dd if=",
      "mkfs",
      ":(){ :|:& };:"
   ],
   "approval_levels": {
      "destructive": "block",
      "write": "allow",
      "read": "allow"
   }
}
```

Before each tool call in `dispatch_tool_call()`, check:
1. Is the tool allowed for the target path?
2. Does the command match any forbidden pattern?
3. Does the side-effect level require approval?

Policies are loaded from the working directory (`.aimee-policy.json`)
or from `~/.config/aimee/policy.json` (global default).

**Affected files:**
- `src/headers/guardrails.h`: `policy_t`, `policy_load()`, `policy_check()`
- `src/guardrails.c`: policy loading and checking
- `src/agent.c`: call `policy_check()` before tool execution

### 5. Deterministic Replay Engine

Record full decision traces for every agent execution:
- Prompt/context hash
- Tool call arguments and results
- Model, provider, temperature
- Memory reads/writes

**Implementation:**

New `execution_trace` table:

```sql
CREATE TABLE execution_trace (
   id INTEGER PRIMARY KEY,
   plan_id INTEGER,
   turn INTEGER NOT NULL,
   direction TEXT NOT NULL,     -- 'request' or 'response'
   content TEXT NOT NULL,       -- full JSON (request body or response body)
   tool_name TEXT,              -- NULL for LLM turns
   tool_args TEXT,              -- NULL for LLM turns
   tool_result TEXT,            -- NULL for LLM turns
   context_hash TEXT,           -- SHA256 of system prompt
   created_at TEXT NOT NULL DEFAULT (datetime('now'))
);
```

In `agent_execute_with_tools()`, log every API request, response, and
tool call to `execution_trace`. New CLI: `aimee trace list`,
`aimee trace show <plan_id>`, `aimee trace replay <plan_id>` (re-executes
the tool calls from the trace without calling the LLM).

**Affected files:**
- `src/db.c`: migration for `execution_trace` table
- `src/agent.c`: trace logging in the agentic loop
- `src/main.c`: `aimee trace` subcommand

### 6. Eval Harness with Task Suites

Ship benchmark tasks and score agent performance.

**Implementation:**

Task suite format (`~/.config/aimee/eval/`):

```json
{
   "name": "health-check",
   "prompt": "Check the health of ack-web via deploy",
   "role": "validate",
   "success_check": {"type": "contains", "value": "Healthy"},
   "max_turns": 5,
   "max_latency_ms": 30000
}
```

New CLI: `aimee eval run [suite]`, `aimee eval results`.

Scorecard tracks: task success rate, avg turns, avg tokens, avg latency,
guardrail violations, rollback frequency. Stored in `eval_results` table.

**Affected files:**
- `src/headers/agent.h`: `eval_task_t`, `eval_result_t`
- `src/agent.c`: `agent_eval_run()`
- `src/db.c`: migration for `eval_results` table
- `src/main.c`: `aimee eval` subcommand

### 7. Confidence Calibration and Abstention

Add confidence scores at decision points. When confidence is low,
the agent abstains (returns to the caller asking for clarification)
instead of acting.

**Implementation:**

In the agentic loop, after each LLM response, check for confidence
signals:
- If the LLM's response includes phrases like "I'm not sure",
  "unclear", "might", the agent flags low confidence.
- If the LLM calls a tool marked `destructive` and the system prompt
  doesn't explicitly authorize it, flag for review.

New fields in `agent_result_t`:

```c
int confidence;    /* 0-100, estimated from response signals */
int abstained;     /* 1 if agent chose not to act */
char abstain_reason[512];
```

When `confidence < 50` and the next action is `write` or `destructive`,
the loop pauses and returns with `abstained = 1` and a reason. The
caller (Claude Code) can then ask the user for confirmation and re-run.

**Affected files:**
- `src/headers/agent.h`: confidence fields in `agent_result_t`
- `src/agent.c`: confidence estimation, abstention logic in loop

### 8. Multi-Agent Coordination

Add coordination primitives for planner/worker/critic patterns:
- Shared blackboard (memory tier L0 visible to all agents in session)
- Quorum voting for risky actions (ask N agents, proceed if majority agree)
- Critic role: after worker proposes, critic reviews before execution

**Implementation:**

New `agent_coordinate()` function that runs a multi-agent flow:

1. **Planner** agent creates a plan (Plan IR from feature 2)
2. **Critic** agent reviews the plan, flags risks
3. **Worker** agent executes the (possibly revised) plan

For quorum voting, `agent_vote()` sends the same prompt to N agents
and returns the majority response.

Shared blackboard: use the existing L0 memory tier. All agents in the
same delegation session share the same `session_id`, so L0 writes from
one agent are visible to others.

**Affected files:**
- `src/headers/agent.h`: `agent_coordinate()`, `agent_vote()` prototypes
- `src/agent.c`: coordination and voting logic
- `src/main.c`: `aimee delegate --coordination planner-critic-worker`

### 9. Artifact-Aware Memory

Link memories to durable artifacts (commits, PRs, files, test runs).
Memories auto-invalidate when their linked artifact changes.

**Implementation:**

New columns on `memories` table:

```sql
ALTER TABLE memories ADD COLUMN artifact_type TEXT;   -- commit, pr, file, test_run
ALTER TABLE memories ADD COLUMN artifact_ref TEXT;    -- sha, PR number, file path
ALTER TABLE memories ADD COLUMN artifact_hash TEXT;   -- content hash at link time
```

When a memory is linked to a file, `artifact_hash` stores the file's
SHA256. During `memory_assemble_context()`, check if the hash still
matches. If not, reduce the memory's confidence (the fact may be stale).

For commits and PRs, check via `git log` / `gh` at session start.

New CLI: `aimee memory link <id> file /path/to/file`,
`aimee memory link <id> commit abc123`.

**Affected files:**
- `src/db.c`: migration adding artifact columns to memories
- `src/memory.c`: artifact linking, staleness detection
- `src/main.c`: `aimee memory link` subcommand

### 10. Grafana Observability Dashboard

Expose agent metrics to Prometheus for visualization on the existing
Grafana server (obs, 192.168.1.100).

**Implementation:**

Add a `/metrics` endpoint to aimee that Prometheus can scrape. Since
aimee is a CLI tool (not a daemon), the approach is to write a metrics
file that a textfile collector reads:

1. After each agent execution, write metrics to
   `/var/lib/prometheus/node-exporter/aimee.prom` in Prometheus
   exposition format.
2. Configure node_exporter's textfile collector on the dev machine
   to pick up the file.
3. Add a Grafana dashboard with panels for:
   - Delegation count by role (counter)
   - Average latency by agent (gauge)
   - Tool calls per delegation (histogram)
   - Token cost per delegation (counter)
   - Success/failure rate (gauge)
   - Confidence distribution (histogram)
   - Rollback frequency (counter)

Metrics file format:

```
# HELP aimee_delegations_total Total delegations by role and agent
# TYPE aimee_delegations_total counter
aimee_delegations_total{role="deploy",agent="codex",status="success"} 5
aimee_delegations_total{role="validate",agent="codex",status="success"} 3

# HELP aimee_delegation_latency_seconds Delegation latency
# TYPE aimee_delegation_latency_seconds gauge
aimee_delegation_latency_seconds{role="deploy",agent="codex"} 4.2

# HELP aimee_tool_calls_total Tool calls by tool name
# TYPE aimee_tool_calls_total counter
aimee_tool_calls_total{tool="bash"} 12
aimee_tool_calls_total{tool="read_file"} 3
```

New Grafana dashboard provisioned via the infrastructure repo's
`08-setup-dashboards.sh`, added alongside the existing Service Health
and Host Utilization dashboards.

**Affected files:**
- `src/agent.c`: `agent_write_metrics()` after each execution
- `infrastructure/homelab/bootstrap/08-setup-dashboards.sh`: new dashboard JSON
- Prometheus config: add textfile collector job for aimee metrics

## Implementation Order

Features are ordered by dependency and build on each other:

1. **Tool Registry** (foundation for validation, policies, and side-effect tracking)
2. **Policy-as-Code** (depends on tool registry side-effect levels)
3. **Plan IR** (depends on tool registry for step planning)
4. **Execution Transactions** (depends on plan IR for rollback strategies)
5. **Replay Engine** (depends on plan IR for trace structure)
6. **Confidence Calibration** (standalone, enhances the agentic loop)
7. **Artifact-Aware Memory** (standalone, enhances memory system)
8. **Multi-Agent Coordination** (depends on plan IR and confidence)
9. **Eval Harness** (depends on replay engine and metrics)
10. **Grafana Dashboard** (depends on metrics from all above)

## Trade-offs

**JSON Schema validation in C vs. external validator:**
Chose inline C. A minimal subset (type, required, enum) is 200 lines of
code and avoids a dependency. Full JSON Schema compliance is unnecessary
for tool argument validation.

**Plan IR as database tables vs. in-memory structs:**
Chose database. Plans need to survive process crashes (background mode)
and support replay. The overhead of SQLite is negligible for plan-sized
data.

**Textfile collector vs. push gateway for metrics:**
Chose textfile collector. Aimee is a CLI, not a daemon. Writing a .prom
file after each run is simpler than maintaining a push gateway connection.
node_exporter's textfile collector is already available on most hosts.

**Policy files in repo vs. central config:**
Chose both. Repo-local `.aimee-policy.json` takes precedence, with
`~/.config/aimee/policy.json` as global fallback. This lets projects
customize safety without requiring every user to configure globally.

## Non-goals

- **Web UI for aimee:** Observability goes through Grafana, not a custom UI.
- **Agent marketplace:** Tool and agent definitions are local config, not
  a shared registry.
- **Real-time streaming of agent output:** The agentic loop runs to
  completion. Streaming is a future consideration.
