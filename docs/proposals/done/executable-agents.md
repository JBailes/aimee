# Executable Agents

## Problem

Aimee's sub-agent system currently delegates text generation to LLM APIs.
Agents receive a prompt, return prose, and the calling AI tool must interpret
and act on the response itself. This means delegation is cosmetic: the caller
still does all the real work (SSH, file operations, validation). There is no
way to offload an actual task, only to offload thinking about a task.

For example, when asked to deploy a service update, the best the caller can do
today is `aimee delegate draft "generate deploy commands"`, read the text
output, then manually run each command. The sub-agent adds latency without
reducing work.

## Goal

Extend the agent system so that sub-agent LLMs can **execute tools** in an
agentic loop. The caller delegates a task, Aimee sends it to the sub-agent
LLM with tool definitions, and manages the loop: the LLM decides what to do,
Aimee executes tool calls locally, sends results back, and repeats until the
LLM completes the task. The caller gets back a structured result with status,
output, and errors.

## Design

### 1. Agentic execution mode

Today `agent_execute()` makes a single API call and returns the text response.
The new mode adds a **tool loop** on top: send messages to the LLM, if the
response contains tool calls then execute them locally and append the results,
repeat until the LLM returns a final response with no tool calls.

```
Caller: aimee delegate deploy "Deploy dashboard fix to obs"
         |
         v
  +------+-------+
  | Build prompt  |  System prompt + tool definitions + user task
  +------+-------+
         |
         v
  +------+-------+
  | API call      |  POST /chat/completions (or /responses)
  +------+-------+
         |
    +----+----+
    |         |
  text     tool_calls
  only     present
    |         |
    v         v
  done    +---+----------+
          | Execute each |  Run bash, read file, write file, etc.
          | tool call    |  locally on this machine
          +---+----------+
              |
              v
          +---+----------+
          | Append tool  |  Add tool results to message history
          | results      |
          +---+----------+
              |
              v
          (loop back to API call)
```

**Loop limits** protect against runaway agents:
- `max_turns`: Maximum tool-call round trips (default: 20).
- `timeout_ms`: Total wall-clock timeout for the entire task (default: 120000).
- Per-tool-call timeout: Inherited from the agent's existing `timeout_ms`
  (applied to each individual bash command, not the LLM API call).

If either limit is hit, Aimee terminates the loop, kills any running
subprocess, and returns a failure result with the partial conversation.

### 2. Tool definitions

Aimee provides the sub-agent LLM with a fixed set of tools via the standard
tool-use format (OpenAI function calling / ChatGPT tool use):

| Tool | Parameters | Description |
|------|-----------|-------------|
| `bash` | `command` (string) | Run a shell command. Returns stdout, stderr, and exit code. |
| `read_file` | `path` (string), `offset` (int, optional), `limit` (int, optional) | Read a file. Returns contents. |
| `write_file` | `path` (string), `content` (string) | Write a file. Returns success/failure. |
| `list_files` | `path` (string), `pattern` (string, optional) | List files matching a glob. Returns file paths. |

**Why these four:**
- `bash` covers SSH, curl, git, service management, and anything else.
  The sub-agent LLM decides what commands to run.
- `read_file` and `write_file` avoid shell quoting issues for file content.
- `list_files` lets the LLM orient itself in a directory.

This is the minimum viable set. Additional tools can be added later without
changing the loop architecture.

**Tool definitions in the API request:**

For OpenAI-compatible APIs, tools are sent as the `tools` array:

```json
{
  "tools": [
    {
      "type": "function",
      "function": {
        "name": "bash",
        "description": "Run a shell command and return stdout, stderr, exit code",
        "parameters": {
          "type": "object",
          "properties": {
            "command": {"type": "string", "description": "The command to execute"}
          },
          "required": ["command"]
        }
      }
    }
  ]
}
```

For the ChatGPT Responses API, the equivalent `tools` format is used per
that API's specification.

### 3. Tool execution

Each tool call from the LLM is executed locally by Aimee:

**`bash`:** Fork a subprocess via `popen()` or `posix_spawn()`. Capture
stdout and stderr separately. Apply a per-command timeout (kill the process
if it exceeds the agent's `timeout_ms`). Return a JSON object:

```json
{
  "stdout": "...",
  "stderr": "...",
  "exit_code": 0
}
```

Stdout and stderr are truncated to 32KB each to avoid blowing up the context
window. If truncated, a note is appended: `"[truncated, showing last 32KB]"`.

**`read_file`:** Direct file read via `fopen()`/`fread()`. Supports optional
`offset` and `limit` (line-based, like the existing Read tool pattern).
Returns file contents or an error message.

**`write_file`:** Direct file write via `fopen()`/`fwrite()`. Returns
`"ok"` or an error message.

**`list_files`:** `glob()` with the given pattern. Returns a newline-separated
list of matching paths, capped at 500 entries.

### 4. Agent config changes

Add two new optional fields to the agent config:

```json
{
  "name": "codex",
  "endpoint": "https://chatgpt.com/backend-api/codex",
  "model": "gpt-5.2-codex",
  "auth_cmd": "/root/.config/aimee/codex-token.sh --access-token",
  "auth_type": "oauth",
  "provider": "chatgpt",
  "roles": ["summarize", "format", "draft", "review", "deploy", "validate", "test", "diagnose"],
  "cost_tier": 0,
  "max_tokens": 4096,
  "timeout_ms": 30000,
  "enabled": true,
  "tools_enabled": true,
  "max_turns": 20
}
```

- `tools_enabled` (bool, default: false): Whether this agent supports tool
  use. When false, the agent behaves exactly as today (single text response).
  When true, tool definitions are included in requests and the agentic loop
  runs.
- `max_turns` (int, default: 20): Maximum tool-call round trips before
  Aimee terminates the loop.

Existing agents are unaffected. Setting `tools_enabled: true` on an agent
opts it into the new execution mode. The same agent can handle both text-only
roles (summarize, draft) and execution roles (deploy, diagnose) depending on
whether the caller provides a role that requires action.

### 5. Role-based tool activation

Tools are not always sent. Aimee decides based on the role:

- **Execution roles** (`deploy`, `validate`, `test`, `diagnose`, `execute`):
  Always include tool definitions if `tools_enabled` is true on the selected
  agent.
- **Text roles** (`summarize`, `format`, `draft`, `review`, `reason`):
  Never include tool definitions, even if `tools_enabled` is true. These
  roles are text-in-text-out by design.

This prevents a `summarize` call from accidentally triggering shell commands,
while allowing the same agent to handle both types of work.

The set of execution roles is configurable per agent via a new optional field:

```json
{
  "exec_roles": ["deploy", "validate", "test", "diagnose", "execute"]
}
```

If omitted, the default set above is used. Any role in `exec_roles` gets
tools; any role not in `exec_roles` does not.

### 6. Context injection

Aimee already maintains rich context about the environment: memory (L2/L3
facts, preferences, decisions), code index (projects, files, exports,
imports), active tasks, and behavioral rules. Today this context is assembled
at session start for the primary AI tool. For executable agents, Aimee
assembles the same context and injects it into the sub-agent's system prompt
so the LLM starts with full situational awareness.

**Context assembly for sub-agents:**

1. **Rules** (from `rules.md`): Behavioral rules the sub-agent must follow.
   These include deployment policies, branch policies, output style, etc.
2. **Relevant memories** (L2/L3): Facts and decisions relevant to the task.
   Aimee runs its existing search against the task prompt to find matching
   memories (same logic as `context_assemble()` at session start).
3. **Repo catalog**: A summary of all indexed projects from the `projects`
   table, including name, root path, and a file map overview. If the task
   references a specific repo, Aimee includes deeper detail for that repo
   (key files, exports, imports, structure from `files`, `file_exports`,
   `file_imports` tables). If the task is broad, Aimee includes the full
   catalog as a directory of available repos so the sub-agent knows what
   exists and where to look.
4. **Active tasks**: Any in-progress tasks from the task graph that relate
   to the delegation.
5. **Environment facts**: SSH targets, service locations, network topology,
   and credentials paths drawn from stored memories.
6. **Network and SSH access**: For agents running on external LLMs (outside
   the local network), Aimee injects connection details so the sub-agent
   knows how to reach the infrastructure. This includes:
   - SSH jump host / VPN endpoint (if applicable)
   - Per-host SSH connection strings (user, IP, port, key path)
   - Network topology (which hosts are on which networks, which are
     reachable from where)
   - Credential paths (SSH key locations, config file locations)

   This information is drawn from stored memories and a new optional
   config section (see below).

**System prompt structure:**

```
You are an execution agent. Complete the task using the provided tools.

# Rules
{rules}

# Context
{context}

# Repos
{repos}

# Active Tasks
{tasks}

# Network Access
{network}

# Instructions
- Use the bash tool to run commands, including SSH to remote hosts.
- When you have completed the task, respond with a final summary.
- If you encounter an error, try to diagnose and fix it. If you cannot
  recover, explain what went wrong.
- Do not ask for confirmation. Execute the task directly.
```

**Budget:** The context injection follows the same budget system as session
start context, but with a higher limit (4000 chars vs. 2000) since sub-agent
tasks are typically more complex and benefit from richer context. The budget
is configurable per agent via an optional `exec_context_budget` field
(default: 4000).

**Why this matters:** Without context injection, the sub-agent LLM would
need to spend tool calls discovering basic facts (what hosts exist, what
services run where, what the deployment process is). Aimee already knows
all of this. Injecting it up front means fewer turns, lower token cost,
and fewer chances for the sub-agent to go off track.

The system prompt template is stored in the agent config as an optional
`exec_system_prompt` field. The template supports `{rules}`, `{context}`,
`{repos}`, `{tasks}`, and `{network}` placeholders that Aimee fills at
delegation time. If omitted, the default template above is used.

**Network config:**

A new optional `network` section in the agent config provides connection
details that Aimee injects into the `{network}` placeholder. This is
particularly important for external LLMs that need to SSH into the homelab
but have no prior knowledge of the network:

```json
{
  "network": {
    "ssh_entry": "ssh -p 2222 deploy@deploy.ackmud.com",
    "ssh_key": "/home/deploy/.ssh/id_ed25519",
    "hosts": [
      {"name": "proxmox", "ip": "192.168.1.253", "user": "root", "desc": "Proxmox VE hypervisor"},
      {"name": "obs", "ip": "192.168.1.100", "user": "root", "desc": "Observability (Grafana, Prometheus, Loki)"},
      {"name": "deploy", "ip": "192.168.1.101", "port": 2222, "user": "deploy", "desc": "Deployment container"}
    ],
    "networks": [
      {"name": "LAN", "cidr": "192.168.1.0/23", "desc": "Home LAN, Grafana, management"},
      {"name": "WOL prod", "cidr": "10.0.0.0/20", "desc": "WOL production services"},
      {"name": "WOL test", "cidr": "10.0.1.0/24", "desc": "WOL test environment"},
      {"name": "ACK", "cidr": "10.1.0.0/24", "desc": "ACK legacy MUD services"}
    ]
  }
}
```

When `{network}` is expanded, Aimee formats this into a readable block:

```
# Network Access
Entry point: ssh -p 2222 deploy@deploy.ackmud.com
SSH key: /home/deploy/.ssh/id_ed25519

Hosts:
  proxmox    192.168.1.253    root     Proxmox VE hypervisor
  obs        192.168.1.100    root     Observability (Grafana, Prometheus, Loki)
  deploy     192.168.1.101:2222 deploy Deployment container

Networks:
  LAN        192.168.1.0/23   Home LAN, Grafana, management
  WOL prod   10.0.0.0/20      WOL production services
  WOL test   10.0.1.0/24      WOL test environment
  ACK        10.1.0.0/24      ACK legacy MUD services
```

The `network` section is optional. If omitted, `{network}` expands to
nothing (for agents running locally that already have direct access).
The host list can also be supplemented by memories: if Aimee has stored
facts about hosts or services, those are merged into the network block.

Note: The `ssh_entry` and `ssh_key` fields in the network config are
templates. At delegation time, Aimee replaces the key path with the
ephemeral key generated for that session (see Safety section). The
sub-agent never sees persistent credentials.

### 7. Structured result

The `agent_result_t` struct gets new fields for execution results:

```c
typedef struct {
   char   agent_name[64];
   char  *response;          /* final text response from LLM */
   int    prompt_tokens;
   int    completion_tokens;
   int    latency_ms;
   int    success;
   char   error[512];
   /* new fields */
   int    turns;             /* number of tool-call round trips */
   int    tool_calls;        /* total tool calls executed */
   char  *conversation;      /* full message history as JSON (for debugging) */
} agent_result_t;
```

**CLI output:**

`aimee delegate deploy "Deploy dashboard fix"` prints the LLM's final text
response to stdout (same as today).

`aimee delegate deploy "Deploy dashboard fix" --json` prints structured JSON:

```json
{
  "status": "success",
  "response": "Deployed successfully. Prometheus reloaded, Grafana restarted.",
  "turns": 4,
  "tool_calls": 6,
  "prompt_tokens": 3200,
  "completion_tokens": 890,
  "latency_ms": 15400
}
```

### 8. Routing integration

No changes to `agent_route()` or the fallback chain logic. Execution mode is
determined after routing:

1. `agent_route()` selects an agent by role and cost (unchanged).
2. `agent_run()` checks: is the role in the agent's `exec_roles` and is
   `tools_enabled` true?
   - Yes: call new `agent_execute_with_tools()` (agentic loop).
   - No: call existing `agent_execute()` (single text response).
3. `agent_run_parallel()` works unchanged. Each thread's `agent_run()` call
   independently decides whether to use tools.

### 9. Database logging

Extend `agent_log` for execution tracking:

```sql
ALTER TABLE agent_log ADD COLUMN turns INTEGER DEFAULT 0;
ALTER TABLE agent_log ADD COLUMN tool_calls INTEGER DEFAULT 0;
```

- `turns`: Number of loop iterations (0 for text-only calls).
- `tool_calls`: Total tool calls executed (0 for text-only calls).

`aimee agent stats` output gains columns:

```
codex  calls=52 tokens=15440/10720 avg=1200ms success=96% turns=3.2avg tools=5.1avg
```

### 10. Safety

**Ephemeral SSH credentials:**

When Aimee provides SSH access to a sub-agent, the credentials must not
outlive the delegation. Aimee generates a short-lived SSH key pair at the
start of each execution session and cleans it up when the session ends
(whether success, failure, or timeout).

All sub-agent SSH access goes through the **deploy container** (CT 101,
192.168.1.101:2222). The deploy container is quad-homed across all four
networks (LAN, WOL prod, WOL test, ACK) and already has SSH open to the
outside world on port 2222. From the deploy container, the sub-agent can
reach any host on any network. This means Aimee only needs to manage one
`authorized_keys` file on one host, not on every target.

Flow:
1. **Before the loop starts:** Aimee generates an ephemeral Ed25519 key
   pair in a temporary directory (`/tmp/aimee-agent-XXXXXX/`).
2. **Authorize the key on deploy:** Aimee appends the ephemeral public
   key to `~deploy/.ssh/authorized_keys` on the deploy container (via
   the user's existing persistent SSH access). The entry is tagged with
   a comment containing the session ID:
   ```
   ssh-ed25519 AAAA... aimee-session-<id>
   ```
3. **Inject connection details:** The sub-agent's `{network}` context
   block tells it to SSH to the deploy container first, then hop to
   target hosts from there using the deploy container's existing keys:
   ```
   Entry: ssh -i /tmp/aimee-agent-XXXXXX/id_ed25519 -p 2222 deploy@<deploy-host>
   From deploy, reach any host via: ssh <user>@<ip>
   (deploy already has SSH keys to all target hosts)
   ```
4. **Sub-agent executes:** The LLM SSHs to the deploy container using
   the ephemeral key, then hops to target hosts using the deploy
   container's own persistent keys. The sub-agent never needs (or
   receives) keys to individual target hosts.
5. **After the loop ends:** Aimee removes the ephemeral public key from
   `~deploy/.ssh/authorized_keys` on the deploy container, then deletes
   the local key pair directory. This runs in a cleanup function that
   executes on all exit paths: normal completion, max_turns, timeout,
   and error.

Since cleanup only touches one host (the deploy container), failure is
unlikely. If it does fail (e.g., deploy container is unreachable), Aimee
logs a warning and stores the session ID so that a subsequent
`aimee agent cleanup` command can retry removal. Stale ephemeral keys
can also be identified and removed by grepping for the `aimee-session-`
comment prefix in the deploy container's `authorized_keys`.

The user's persistent SSH keys are never exposed to the sub-agent LLM.
The sub-agent only ever sees the ephemeral key path. Target host keys
stay on the deploy container and are never transmitted to the LLM.

**Command allowlist/denylist (optional):**
An optional `exec_allowed_commands` array in agent config restricts what
bash commands the sub-agent can run. If empty (default), all commands are
allowed. If populated, Aimee checks that the command starts with an allowed
prefix before execution. Denied commands return an error tool result to the
LLM (not a hard failure, letting it try an alternative).

```json
{
  "exec_allowed_commands": ["ssh", "scp", "curl", "git", "dotnet", "make", "systemctl"]
}
```

**Path restrictions (optional):**
An optional `exec_allowed_paths` array restricts `read_file`, `write_file`,
and `list_files` to specified directory prefixes. If empty (default), all
paths are allowed.

```json
{
  "exec_allowed_paths": ["/root/aicli", "/tmp"]
}
```

**Guardrail integration:**
The existing aimee guardrail system (sensitive file detection, blast radius
checks) applies to tool calls from sub-agents the same way it applies to
tool calls from the primary AI tool. Sub-agent file writes go through the
same `classify_path()` check.

## Affected Files

| File | Change |
|------|--------|
| `src/headers/agent.h` | Add `tools_enabled`, `max_turns`, `exec_roles`, `exec_system_prompt`, `exec_allowed_commands`, `exec_allowed_paths` to `agent_t`. Add `turns`, `tool_calls`, `conversation` to `agent_result_t`. Add `agent_execute_with_tools()` prototype. |
| `src/agent.c` | Implement `agent_execute_with_tools()` (agentic loop). Implement tool execution functions (`tool_bash()`, `tool_read_file()`, `tool_write_file()`, `tool_list_files()`). Update `agent_run()` dispatch logic. Update config load/save. Update stats display. |
| `src/agent_http.c` | Update request builders to include `tools` array when in execution mode. Update response parsers to extract `tool_calls` from LLM responses. |
| `src/main.c` | Add `--json` flag to delegate/run commands. Update `agent add` for new config fields. |
| `src/migrations.c` | Add migration for `turns` and `tool_calls` columns on `agent_log`. |
| `src/tests/test_agent.c` | Add tests for tool execution, loop termination, output truncation, and safety checks. |

## Trade-offs

**Fixed tool set vs. plugin tools:**
Chose fixed. Four tools (bash, read_file, write_file, list_files) cover
virtually all execution needs. `bash` alone is sufficient for most tasks;
the file tools exist to avoid shell quoting edge cases. A plugin system
would add complexity without clear value at this stage.

**Role-based tool activation vs. always-on tools:**
Chose role-based. Prevents a `summarize` call from accidentally running
shell commands. The caller controls whether tools are available by choosing
the role, and the agent config controls which roles get tools.

**Per-command allowlist vs. unrestricted execution:**
Made the allowlist optional. Default is unrestricted (the sub-agent has the
same permissions as the user). The allowlist exists for users who want to
constrain sub-agents, but requiring it by default would make simple setups
harder.

**Full conversation in result vs. summary only:**
Included optional full conversation (`--json` flag). For debugging and audit,
seeing every tool call the LLM made is essential. For normal use, only the
final response is shown.

## Non-goals

- **Streaming output:** The agentic loop runs to completion before returning.
  Real-time streaming of tool calls to the caller is a future consideration.
- **Multi-agent orchestration:** One agent per delegation. The caller can
  delegate multiple tasks in parallel via `agent parallel`, but agents do
  not delegate to each other.
- **Persistent agent sessions:** Each delegation is stateless. The LLM gets
  the task prompt and tool definitions fresh each time. There is no
  conversation memory between delegations.
- **Scheduling/cron:** Aimee does not become a scheduler.
- **Secrets management:** Sub-agents use the same SSH keys and credentials
  available to the user.
