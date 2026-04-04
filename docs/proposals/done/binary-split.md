# Proposal: Server + Client Architecture

## Problem

aimee is a single binary where every component has direct database access, guardrails are enforced in-process (any new frontend that doesn't call the checks silently bypasses them), and all concerns — state management, policy enforcement, compute execution — are compiled into one monolith. This creates three classes of problems:

1. **No isolation.** Adding frontends (webchat, Discord) means either embedding them in the monolith or giving each binary raw DB access with no scoping. You can't update the CLI without stopping the web server.

2. **No central authority.** Guardrails depend on each binary calling the right checks. A new client that skips `guardrails.c` gets unrestricted access. The LLM agent loop decides which tools to call based on untrusted model output, and the executing binary runs whatever it asks.

3. **No multi-tenancy.** Process-global state (`session_id()`, prepared statement cache, curl init) assumes one session per process. The codebase cannot serve multiple clients concurrently without substantial refactoring.

## Architecture

One server process owns all state and enforces all policy. Everything else is a client.

```
                aimee-server
                (state, policy, compute)
                (single process, owns DB exclusively)
                      |
               Unix socket
          ~/.config/aimee/aimee.sock
                      |
      +---------------+---------------+---------------+
      |               |               |               |
    aimee          webchat          discord          future
    (CLI)          (HTTPS)          (bot)           clients
```

### Internal separation of concerns

The server is one process but internally separates three domains with distinct threading and resource models:

**State layer** — memory, index, rules, sessions, config. Serves short-lived read/write queries. Executes inline on the event loop or in a small fast-path thread pool. Target latency: <5ms.

**Policy layer** — guardrails, capability checks, session scoping, audit logging. Synchronous, inline, never blocks on I/O. Runs as part of every request before dispatch to compute.

**Compute layer** — LLM API calls, tool execution (bash, file I/O), delegates, agentic loops. Long-running. Executes in a bounded worker thread pool, separate from state queries. A 3-minute delegate never blocks a 1ms memory lookup.

This prevents a compute stampede from making the CLI unresponsive.

### Layered libraries

Core logic is split into four layered static libraries. Each layer depends only on layers below it, enabling binaries to link only what they need.

```
libaimee-core.a   (Layer 0)  db, config, util, text, render, cJSON
       |
libaimee-data.a   (Layer 1)  memory, index, rules, tasks, guardrails, workspace, extractors
       |
libaimee-agent.a  (Layer 2)  agent loop, tools, HTTP client, policy, plan, eval, coordination
       |
libaimee-cmd.a    (Layer 3)  cmd_* handlers, webchat, dashboard
```

```makefile
aimee:         cli_main.o cli_client.o cJSON.o                          # 132KB, no sqlite/curl
aimee-mcp:     mcp_server.o libaimee-data.a libaimee-core.a             # 443KB, sqlite only
aimee-server:  server_*.o libaimee-cmd.a ...-agent.a ...-data.a ...-core.a  # 1.2MB, full deps
aimee-direct:  main.o cli_client.o libaimee-cmd.a ...-agent.a ...-data.a ...-core.a  # 1.7MB
```

`cli_client.o` is a shared client library for Unix socket communication — any client links it.

## Protocol

Newline-delimited JSON over the Unix socket. No HTTP, no protobuf — any language can implement a client.

### Schema

Every message has a defined schema. Required and optional fields, types, and constraints are specified per method in a machine-readable format (JSON Schema). Server dispatch and client stubs are generated from the schema where possible.

**Envelope:**

```json
{
  "method": "string (required)",
  "request_id": "string (optional, required for mutating methods)",
  "deadline_ms": "int (optional, server enforces default per method class)",
  ...method-specific fields...
}
```

**Response:**

```json
{
  "status": "ok | blocked | error | overloaded | degraded | forbidden",
  "request_id": "string (echoed if provided)",
  ...method-specific fields...
}
```

**Streaming:**

```json
{"event": "token | chunk | done | error | overflow", "data": "..."}
```

Event ordering is guaranteed: tokens arrive in order, `done` or `error` is always the last event. No events after `done`/`error`. On buffer overflow (`overflow` event), `dropped` count is included.

**Constraints:**
- Max message size: 16MB
- Max JSON depth: 32
- Session IDs: alphanumeric + hyphens + colons, max 64 chars, server-generated only
- All string fields have defined max lengths per method

### API evolution

New methods are additive — old clients ignore them. New response fields are additive — clients must ignore unknown fields. Removing a method or changing a field type is a breaking change requiring a protocol version bump. Deprecated methods include `"deprecated": true` in responses for 2 versions before removal.

### Methods

```
# Connection
auth                    # present credentials, receive capabilities
server.info             # protocol version, server version, schema version
server.health           # uptime, DB status, agent status, degraded reasons
server.limits           # current resource limits and usage

# Sessions (server-generated IDs, scoped to principal)
session.create          # create session, returns server-assigned ID
session.list            # list sessions visible to this principal
session.get             # get session details
session.close           # close session, release resources
session.update          # update mutable session fields (e.g. title)
session.start           # initialize session (worktrees, context assembly)

# Chat / agent execution
chat.send               # send message, get response
chat.send_stream        # send message, stream tokens back
chat.history            # conversation history for a session

# Delegation
delegate                # delegate to sub-agent (sandboxed, guardrails enforced)
delegate.status         # check background task status
task.cancel             # cancel a running async task

# Tool execution (sandboxed, guardrails enforced)
tool.execute            # execute a tool call (bash, read, write, etc.)
tool.validate           # dry-run guardrail check without executing

# Memory
memory.search           # search memories
memory.store            # store a memory (requires memory.write capability)
memory.list             # list memories by tier/kind

# Index
index.find              # find symbol definitions
index.scan              # trigger re-index (requires index.admin capability)
index.blast_radius      # check file impact

# Rules
rules.list              # list active rules
rules.generate          # generate rules text (requires rules.admin capability)

# Describe
describe.read           # read a project description
describe.run            # trigger re-describe (requires describe.admin capability)

# Dashboard
dashboard.delegations   # recent delegations
dashboard.metrics       # agent metrics
dashboard.logs          # activity log

# Workspace
workspace.context       # get workspace context string

# Hooks (AI tool integration)
hooks.pre               # pre-tool guardrail check
hooks.post              # post-tool processing
```

### Multiplexing

The initial implementation does not support multiplexing — each connection handles one request at a time. Clients that need concurrent operations (webchat proxying multiple user actions) open multiple connections. Connection limits account for this. A future protocol version may add request/response `id` tagging for multiplexing over a single connection.

## Authentication and Capabilities

### Connection authentication

The server determines client identity from the connection itself — never from client self-declaration.

1. On every accepted connection, verify `SO_PEERCRED` before processing any data. Drop connections with unverifiable credentials immediately.
2. For local same-UID connections: baseline local trust. To get full CLI capabilities, the client must also present a **capability token** — a server-issued secret stored in a root-owned file (`~/.config/aimee/cli.token`, mode `0600`). This prevents any same-UID process from automatically getting full access.
3. For webchat: the webchat process performs PAM auth itself, then presents a server-issued session token (obtained after the server verifies the webchat process identity via `SO_PEERCRED`). PAM credentials never traverse the socket.
4. For Discord: presents a bot token that the server validates against stored config.

Trust levels become shorthand for default capability sets, not the enforcement mechanism:

| Level | Client | Default auth |
|-------|--------|-------------|
| `local_unattested` | Same UID, no token | Reduced capabilities (read-only state, hooks) |
| `local_attested` | Same UID + capability token | Full capabilities |
| `authenticated` | Webchat (PAM'd user) | Scoped capabilities |
| `limited` | Discord (bot token) | Minimal capabilities |

### Capability model

Instead of coarse trust levels, the server assigns capabilities per connection:

```
chat, delegate, tool.execute, tool.bash, tool.write,
memory.read, memory.write, rules.read, rules.admin,
describe.read, describe.admin, index.read, index.admin,
session.read, session.admin, dashboard.read
```

Each client type gets a default capability set. Specific sessions can have capabilities added or revoked. The server checks capabilities per-method — a method that requires `tool.bash` is rejected if the connection lacks that capability, regardless of trust level.

**Default capability sets:**

| Capability | CLI (attested) | CLI (unattested) | Webchat | Discord |
|-----------|----------------|------------------|---------|---------|
| `chat` | yes | yes | yes | yes |
| `delegate` | yes | no | yes | yes |
| `tool.execute` | yes | no | yes (sandboxed) | no |
| `tool.bash` | yes | no | restricted | no |
| `tool.write` | yes | no | worktree only | no |
| `memory.read` | yes | yes | yes | yes |
| `memory.write` | yes | no | yes | no |
| `rules.admin` | yes | no | no | no |
| `describe.admin` | yes | no | no | no |
| `index.admin` | yes | no | no | no |
| `session.admin` | yes | no | no | no |
| `dashboard.read` | yes | yes | yes | yes |

### Auth rate limiting

After N failed auth attempts (e.g. 5) from the same source within a window (e.g. 1 minute), reject further attempts for a cooldown period (e.g. 5 minutes). Log all auth failures with source identity. Per-user session quotas (e.g. max 3 per user) prevent session slot exhaustion attacks.

### Session scoping

Sessions are server-created and server-owned:

1. Client calls `session.create` → server generates a canonical ID, records `{id, client_type, principal, created_at}`
2. Client references sessions by server-assigned ID
3. Server validates on every request: does this session belong to this principal? Reject with `forbidden` if not.
4. `session.admin` capability (CLI) can reference any session. Others can only see their own.

### Webchat multi-session

Webchat supports multiple concurrent sessions per authenticated user, presented as tabs in the UI. Each tab is an independent conversation with its own Claude CLI session, worktree, and conversation history.

**Lifecycle:**

1. User opens webchat → frontend calls `session.list` to restore any existing sessions for this principal
2. User clicks "new tab" → webchat calls `session.create`, opens a new tab bound to the returned session ID
3. Each tab sends messages via `chat.send_stream` scoped to its session ID
4. User closes a tab → webchat calls `session.close`, server releases the session's resources (Claude CLI session, worktree)
5. Server reaps idle sessions after 48 hours with no activity (`last_activity_at` tracked per session)

**Session record:**

```json
{
  "id": "server-generated",
  "client_type": "webchat",
  "principal": "username",
  "created_at": "...",
  "last_activity_at": "...",
  "claude_session_id": "...",
  "title": "auto-generated or user-set"
}
```

**Persistence:** Conversations persist server-side until expiry or explicit close. The server stores conversation history per session (via `chat.history`). The frontend no longer owns persistence — localStorage is a cache for fast rendering, not the source of truth. On page reload or reconnect, the frontend calls `session.list` to get active sessions and `chat.history` per session to restore tab contents.

**Tab title:** Auto-generated from the first user message (truncated to 50 chars). Users can rename via `session.update` (new method, updates mutable session fields like title).

**Expiry:** A background reaper runs on a timer (e.g. every 15 minutes), queries sessions where `now - last_activity_at > 48h`, and closes them (releasing Claude CLI session, cleaning up worktree). `chat.send_stream` and `chat.send` update `last_activity_at` on every call.

**Limits:** Per-user session cap (e.g. 10 active webchat sessions). `session.create` returns `overloaded` if the cap is hit. The user must close an existing tab before opening a new one.

## Guardrails

### Centralized enforcement

Every tool execution goes through the server's policy layer. The sequence is atomic:

1. Acquire read lock on rules/policy state
2. Validate input (field types, path safety, max lengths)
3. Check capabilities — does this connection have the required capability?
4. Classify target — file path risk, blast radius
5. Check plan mode — session in plan mode blocks all writes
6. Check anti-patterns — compare against known-bad patterns
7. Check drift — warn/block if operation is out of scope for active task
8. Enforce worktree isolation — writes go to session worktree, not main tree
9. If all pass, execute the tool (still holding read lock)
10. Commit result to database
11. Release lock
12. Return result

The read lock prevents rule changes from taking effect mid-check (TOCTOU protection). Multiple concurrent guardrail checks are fine; rule mutations wait for the write lock.

### Agent sandbox

The LLM agent loop decides which tools to call based on model output — it's untrusted. Before the agentic loop starts, the server defines an **agent sandbox** based on the connection's capabilities and session context:

- The sandbox is narrower than the connection's full capability set
- A webchat agent can read files but only write to the session's worktree
- Bash commands are restricted to an allowlisted command set per capability level
- The sandbox is enforced at the `dispatch_tool_call` layer, not just in guardrails
- This is an allowlist (permit known-good), not a denylist (block known-bad)

### Advisory vs enforced

| Path | Executor | Enforcement |
|------|----------|-------------|
| Claude/Gemini/Codex tools | AI tool | Advisory — server says allow/block via hooks, AI tool respects exit code |
| Aimee delegates | aimee-server | Enforced — server controls execution, agent sandboxed |
| Webchat chat | aimee-server | Enforced |
| Discord chat | aimee-server | Enforced |

For advisory paths: optional post-tool reconciliation can detect violations (guardrail said allow, but outcome was harmful) and block continuation. Telemetry tracks pre-allow/post-violation deltas.

### Audit log

Every guardrail decision is an immutable audit event:

```json
{
  "event": "guardrail_decision",
  "request_id": "...",
  "session_id": "...",
  "client_id": "...",
  "timestamp": "...",
  "method": "tool.execute",
  "tool": "write_file",
  "path": ".env",
  "decision": "blocked",
  "reason": "sensitive_file",
  "severity": "red"
}
```

Audit events also cover: auth decisions, tool execution intents/results, config/rule mutations, session lifecycle. Events include payload hashes for forensic traceability. Append-only — no update or delete path. Retention and rotation policy integrates with journald.

## Concurrency

### Threading model

Event loop (epoll) for connection management and request parsing. Two bounded worker pools:

- **Fast pool** (state queries): memory lookups, index searches, rule checks, session operations. Short-lived, <5ms target.
- **Compute pool** (long-running): delegates, chat with tool use, bash execution, LLM API calls. Minutes-scale.

Short operations execute inline on the event loop. Blocking work dispatches to the appropriate pool. This prevents compute starvation of state queries.

### Resource limits

| Resource | Limit |
|----------|-------|
| Max client connections | 64 |
| Max connections per client type | 32 CLI, 16 webchat, 1 Discord |
| Max webchat sessions per user | 10 |
| Max concurrent compute jobs (global) | 20 |
| Max concurrent compute jobs per client | 5 |
| Max streaming sessions per client | 3 |
| Max queued requests per client | 16 (then `overloaded` rejection) |
| Send buffer per connection | 256KB |
| Default deadline: state queries | 5s |
| Default deadline: tool.execute | 30s |
| Default deadline: delegate/chat | 180s |

Per-client rate limits on expensive methods (delegate, chat.send, describe.run): token bucket with configurable budget per capability level.

### Backpressure and streaming

- If a connection's send buffer fills (slow consumer), server drops oldest unread events and sends `{"event":"overflow","dropped":N}` when the client catches up.
- If the buffer stays full for 30 seconds, close the connection and cancel the associated task.
- Producer (LLM callback) blocks briefly (100ms) when buffer is full, then drops if still full. Prevents OOM without deadlock.

### Database concurrency

- SQLite in WAL mode with 5-second busy timeout (already configured)
- Each client handler thread opens its own `sqlite3 *` connection with its own prepared statement cache — no shared cache, no mutex needed
- Server main thread holds one connection for background tasks (session pruning, index updates)
- Writes are serialized by SQLite's WAL write lock — no application-level locking needed for DB mutations
- Per-method transaction mode: `DEFERRED` for reads, `IMMEDIATE` for writes
- Index scans serialized by a server-side mutex (one scan at a time, concurrent reads fine)
- Guardrail state (rules, anti-patterns) protected by read-write lock per the atomic check sequence above

### Concurrency invariants

Lock ordering (when multiple locks needed): `config_rwlock` → `rules_rwlock` → `index_scan_mutex`. No other combinations. Document and assert.

## Failure Model

### Execution guarantees

| Operation | Guarantee | On crash |
|-----------|-----------|----------|
| Chat/delegate | Best-effort, no resume | Client gets connection error, retries from scratch |
| Tool execution (write_file, bash) | At-most-once | Partial writes possible; checkpoints lost (see recovery) |
| Memory/rules writes | Idempotent by key | Dedup prevents double-apply |
| Streaming | Ordered delivery, may drop on overflow | Client reconnects, restarts stream |

### Idempotency

Mutating methods accept an optional `request_id`. If provided:
- Server stores `{request_id, result, timestamp}` in a `request_log` table
- Duplicate `request_id` within the idempotency window (5 minutes) returns the cached result without re-executing
- Read-only methods don't need `request_id`

### Cancellation

`task.cancel` by task ID or request ID:
- Sends SIGTERM to the worker thread's subprocess (if bash tool)
- Marks task as cancelled in database
- Cancellation propagates to DB transaction scopes (rollback incomplete transactions)
- Only the task owner or `session.admin` can cancel

### Crash recovery

On startup after crash:
- Scan for in-flight tasks (status = "running") and mark as "failed" with crash-recovery reason
- Clients polling for status see the failure and can retry
- In-process checkpoints (`agent_tools.c`) are lost — long-term, persist to DB for rollback capability
- Stale socket: `lstat()` the path, verify `S_ISSOCK`, attempt connect. If no server responding, unlink and proceed. Reject symlinks (lstat vs stat mismatch).

### Server health and readiness

```
server.health  → {"status":"ok","uptime":1234,"db":"ok","agents":"ok"}
server.ready   → ok only after full initialization
```

Use systemd `Type=notify` with `sd_notify(READY=1)` after socket is listening and DB is open.

**Degraded mode:** If DB is locked/corrupt, return `{"status":"degraded","reason":"db_locked"}` for DB-dependent methods while read-only methods continue. If all LLM backends are unreachable, chat/delegate return `{"status":"degraded","reason":"no_agents"}`.

### Graceful shutdown

On SIGTERM:
1. Stop accepting new connections
2. Stop accepting new requests on existing connections
3. Wait for in-flight bounded operations (configurable drain timeout, e.g. 30s)
4. Cancel remaining operations
5. Close DB connections
6. Unlink socket
7. Exit

## Deployment

### Privilege model

The server runs as a dedicated system user, not root:

```bash
useradd --system --home-dir /var/lib/aimee --shell /usr/sbin/nologin aimee
```

Database, config, socket, TLS certs owned by `aimee` user. CLI runs as the calling user and connects to the socket. Server verifies calling UID via `SO_PEERCRED`.

For tool execution as the calling user: server uses `sudo -u <caller>` with the UID from `SO_PEERCRED`. This is more secure than running the server as root.

### Systemd services

```ini
# /etc/systemd/system/aimee-server.service
[Unit]
Description=Aimee Server
After=network.target

[Service]
Type=notify
User=aimee
Environment=HOME=/var/lib/aimee
ExecStart=/usr/local/bin/aimee-server
Restart=on-failure
RestartSec=5
NoNewPrivileges=yes
PrivateTmp=yes
ProtectSystem=strict
ProtectHome=read-only
ReadWritePaths=/var/lib/aimee
RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6

[Install]
WantedBy=multi-user.target
```

```ini
# /etc/systemd/system/aimee-webchat.service
[Unit]
Description=Aimee Webchat
After=aimee-server.service
Requires=aimee-server.service

[Service]
Type=simple
User=aimee-webchat
ExecStart=/usr/local/bin/aimee-webchat --port 8080
Restart=on-failure
RestartSec=5
NoNewPrivileges=yes
PrivateTmp=yes
ProtectSystem=strict

[Install]
WantedBy=multi-user.target
```

### Socket hardening

- Create with `umask(0077)` → permissions `0600`
- Parent directory `~/.config/aimee/` enforced `0700`
- Before bind: `lstat()` path, require `S_ISSOCK` if exists, reject symlinks
- Atomic startup lockfile prevents dual-server races
- Readiness signal emitted only after bind/listen/chmod succeed

### ACL defaults

Webchat binds to loopback only (`127.0.0.0/8`) by default. Non-loopback networks require explicit allowlist configuration. Warning logged at startup if non-loopback networks are in the ACL.

### Install flow

```bash
./install.sh
  1. Build all binaries (aimee, aimee-server, aimee-webchat)
  2. Create aimee system user if needed
  3. Install to /usr/local/bin/
  4. Create database and config with correct ownership
  5. Generate capability token for CLI
  6. Install aimee-server.service, enable, and start
  7. Done — aimee CLI works immediately
```

Webchat and other frontends are opt-in:

```bash
aimee webchat enable     # installs and starts aimee-webchat.service
aimee discord enable     # installs and starts aimee-discord.service (future)
```

### Updates

Each component is independently deployable:

```bash
# CLI — not a daemon, just replace the binary
cp aimee /usr/local/bin/

# Webchat — restart its service, server unaffected
systemctl stop aimee-webchat && cp aimee-webchat /usr/local/bin/ && systemctl start aimee-webchat

# Server — dependent services restart automatically via Requires=
systemctl stop aimee-server && cp aimee-server /usr/local/bin/ && systemctl start aimee-server
```

Server restart is sub-second. Graceful drain ensures in-flight operations complete before shutdown. Future: systemd socket activation for zero-downtime upgrades.

### Versioning

Three version axes: server version, protocol version, schema version.

- Server stores `{server_version, schema_version, protocol_version}` in `server_info` table
- Clients query `server.info` on connect; protocol version mismatch → actionable error
- On startup, server checks schema version: older → auto-migrate; newer → refuse to start with clear error
- Compatibility matrix published for `{CLI × Server × Webchat}` supported combinations

## CLI Context Forwarding

When Claude Code (or Gemini/Codex) calls aimee hooks, the CLI forwards calling context to the server:

```
Claude Code runs: aimee hooks pre
                       |
                   aimee CLI
                   1. Reads stdin (tool name, args from Claude)
                   2. Captures CWD, env vars (CLAUDE_SESSION_ID, etc.)
                   3. Connects to server socket, presents capability token
                   4. Sends: {"method":"hooks.pre",
                              "request_id":"...",
                              "cwd":"/root/aimee/wol-client",
                              "session_id":"abc123",
                              "stdin":"Write tool editing .env",
                              "env":{"CLAUDE_TOOL_NAME":"Write"}}
                   5. Server runs policy layer (all guardrail checks)
                   6. Server responds: {"status":"blocked","reason":"sensitive file"}
                   7. CLI writes reason to stderr, exits with code 2 (block)
```

Latency: Unix socket round-trip <1ms. Current hook path forks+execs a process (~10ms), so the socket call replaces, not adds to, the overhead.

After `aimee` with no args execs into `claude` (replacing the process), each subsequent hook call is a fresh connect → request → disconnect cycle. No persistent connection needed.

## Observability

### Metrics

Exposed via `server.metrics` method (Prometheus text format) and the existing textfile collector:

- Requests/sec by method
- Latency histograms (p50, p95, p99) by method class (state, compute)
- Active connections by client type
- Agent token usage by provider
- Guardrail block rate by severity
- Error rate by status code
- Worker pool utilization (fast pool, compute pool)

### Tracing

Request ID flows from client → server → LLM call → tool execution → response. Connected to existing `execution_trace` table. Each trace entry includes: request_id, session_id, client_id, method, duration, outcome.

### Anti-automation

Auth endpoints log structured events: `{event, user, source, timestamp, attempt_count}`. Velocity and pattern anomaly detection for credential stuffing.

## Required Refactoring

Before or during implementation, the existing codebase needs these changes:

1. **Remove `session_id()` as global.** Add session_id parameter to every function that uses it: `session_state_path()`, `wm_assemble_context()`, `agent_build_exec_context()`, all of `cmd_hooks.c` and `guardrails.c`.

2. **Explicit CWD for worktrees.** Worktree creation and path resolution must accept a base directory from the client, not derive from `getcwd()`.

3. **Single `agent_http_init()`.** Call once at server startup before any threads. Remove all other call sites. Per-thread curl handles are already the case.

4. **Per-thread DB connections.** Each client handler opens its own `sqlite3 *` with its own statement cache. No shared cache.

5. **Config load-once.** Load at startup, reload on SIGHUP with read-write lock.

6. **Replace fork with threads.** `cmd_describe.c` uses `fork()` for parallelism — replace with pthreads in the server.

7. **Global state audit.** `grep -rn 'static ' src/*.c` for all mutable static state. Classify each as: thread-safe, needs mutex, needs per-thread instance, or needs removal. Document results. Signal handlers must be async-signal-safe.

8. **Session restore integrity.** Bind restore tokens to authenticated session identity. HMAC-sign or store server-side; never trust client-provided values without verification.

## Migration

### Strategy

Incremental, with rollback at every step. The monolith continues to work throughout.

1. Build `server.c` and `cli_client.c` alongside the monolith
2. CLI detects server availability: if socket exists and responds, use server; otherwise fall back to direct mode
3. Migrate commands one at a time: hooks first (highest value), then memory, then delegates
4. Behavioral equivalence tests: for each migrated command, run via monolith and via server, diff output
5. Feature flag per command (`server_enabled_commands` in config)
6. Remove monolith fallback only after all commands pass equivalence tests for sustained period

### Rollout gates

- **Gate 1**: Protocol compatibility tests (CLI ↔ server round-trip for all methods)
- **Gate 2**: Guardrail parity tests (monolith vs server produce identical allow/block decisions)
- **Gate 3**: Load/concurrency tests (N concurrent clients, mixed workloads)
- **Gate 4**: Failure injection tests (crash mid-delegate, restart during streaming, stale socket on startup)

### Read timeouts

All connections have read timeouts: 30s for header parsing, 60s for body. Minimum read-rate enforcement — connections sending less than N bytes in M seconds are closed (Slowloris mitigation).

## Changes

| File | Change |
|------|--------|
| `server.c` | New: event loop, worker pools, method dispatch, policy layer, audit logging |
| `server.h` | New: server types, capability definitions, API schema |
| `server_main.c` | New: server entry point, socket lifecycle, signal handling, readiness |
| `cli_client.c` | New: shared client library for socket communication |
| `cli_client.h` | New: client types and connection API |
| `cli_main.c` | New: CLI entry point (thin client with fallback) |
| `webchat_main.c` | New: webchat entry point (HTTPS + server client) |
| `webchat.c` | Refactor to proxy through server |
| `config.c` | Remove `session_id()` global, add per-call session parameter |
| `guardrails.c` | Accept session context as parameter, not global |
| `agent_context.c` | Accept session context as parameter |
| `working_memory.c` | Accept session context as parameter |
| `cmd_describe.c` | Replace fork with pthreads |
| `db.c` | Per-connection statement cache (remove global) |
| `Makefile` | Build layered libraries, server, and client binaries |
| `systemd/aimee-server.service` | New: hardened server service |
| `systemd/aimee-webchat.service` | Update: depends on server, hardened |
| `install.sh` | Create system user, install all binaries, start server |

## Non-Goals

- No HTTP/REST API on the server. Protocol is Unix socket + JSON lines. A web API would be a thin wrapper over the same socket protocol — built as a separate frontend if needed.
- No remote access to the server. Unix socket is local only. Remote access goes through frontends (webchat HTTPS, Discord gateway).
- No immediate Discord implementation. The architecture makes it a thin client when ready.
- No runtime plugin loading. Frontends are compile-time binaries.
