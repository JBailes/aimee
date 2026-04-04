# Proposal: CLI Thin-Client Routing for Sub-10ms Delegate Calls

## Problem

Delegates frequently shell out to `aimee memory`, `aimee rules`, `aimee delegate`, etc. Each invocation currently pays 70-250ms of per-process overhead before doing any real work:

| Operation | Cost | Where |
|-----------|------|-------|
| `db_open()` — full migrations check (28 versions) + FTS population | 50-150ms | `db.c:729-778` |
| `agent_http_init()` — `curl_global_init()` | 1-50ms | `agent_http.c:163` |
| `agent_build_exec_context()` — `rules_generate()` + memory FTS search | 20-100ms | `agent.c` / `agent_context.c` |
| Process startup — dynamic linking, libc init | 2-5ms | OS |

**Total per-invocation: 70-250ms.** A delegate doing 10 memory lookups pays 0.7-2.5s in pure overhead.

The server already has native RPCs for everything delegates need (`memory.search`, `memory.store`, `memory.list`, `memory.get`, `rules.generate`, `rules.list`, `delegate`, `wm.set`, `wm.get`, `index.find`), with the DB already open and curl initialized. But no CLI subcommand uses them — `aimee memory search foo` calls `cmd_memory()` which calls `db_open()` in-process. Even `cli.forward` (the generic server-side exec) forks `aimee-direct` as a child process (`server_forward.c:193`), paying full startup again.

Hooks are the sole exception: `main.c:256-259` routes them through `cli_hooks_via_server()` when a server is available.

## Goals

- `aimee memory search <query>` completes in <10ms CLI overhead (plus server processing time).
- `aimee rules generate` completes in <10ms CLI overhead.
- `aimee delegate <role> "prompt" --background --json` returns task_id in <10ms.
- No behavioral change when server is unavailable — fall back to in-process execution.
- Delegates inherit `AIMEE_SOCK` from the parent session and hit a warm server on every call.

## Approach

### Route CLI subcommands through server RPCs (M)

Generalize the hooks pattern (`main.c:256-259`) to all commands that have server-side RPC equivalents. The CLI becomes a thin client: connect to socket, send JSON, print response.

**Design:**

Add a lookup table mapping CLI subcommands + sub-subcommands to server RPC methods:

```c
static const struct {
    const char *cmd;        /* CLI subcommand */
    const char *subcmd;     /* CLI sub-subcommand (NULL = any) */
    const char *method;     /* server RPC method */
} rpc_routes[] = {
    {"memory",   "search",   "memory.search"},
    {"memory",   "store",    "memory.store"},
    {"memory",   "list",     "memory.list"},
    {"memory",   "get",      "memory.get"},
    {"rules",    "list",     "rules.list"},
    {"rules",    "generate", "rules.generate"},
    {"wm",       "set",      "wm.set"},
    {"wm",       "get",      "wm.get"},
    {"wm",       "list",     "wm.list"},
    {"index",    "find",     "index.find"},
    {"index",    "list",     "index.list"},
    {"delegate", NULL,       "delegate"},
    {NULL, NULL, NULL}
};
```

In `main()`, before the command table lookup:

```c
if (cli_server_available(NULL)) {
    const char *method = cli_rpc_for_command(cmd, sub_argv[0]);
    if (method)
        return cli_rpc_forward(&ctx, method, sub_argc, sub_argv);
}
```

`cli_rpc_forward()` does:
1. Connect to `AIMEE_SOCK` or well-known socket (~1ms)
2. Authenticate if needed (~0.5ms)
3. **Version handshake:** include `"protocol_version": 1` in the request. Server validates that it supports the requested version. If the server returns `"error": "unsupported protocol version"`, the CLI falls back to in-process execution. (~0.1ms, piggybacks on the auth exchange)
4. Marshal argv into JSON request (~0.1ms)
5. Send request, read response (~0.1ms + server processing)
6. Print result to stdout (~0.1ms)

**Total CLI overhead: ~2-3ms.**

### RPC compatibility contract

The CLI and server are built from the same source and ship as a single binary (`aimee` + `aimee-server`). Version skew only occurs when:
- A persistent server (systemd) is running an older binary while the CLI has been upgraded.
- A non-persistent server was auto-started by an older session and a newer CLI connects to it.

**Strategy:**
- Each RPC request includes `"protocol_version": N` (integer, starts at 1).
- The server checks `protocol_version` against its supported range. If unsupported, it returns an error with `"min_version"` and `"max_version"` fields.
- The CLI falls back to in-process on version mismatch — no silent behavior drift.
- `server.info` already returns server metadata; add `protocol_version` to its response so the CLI can detect skew early during `cli_server_available()`.
- Protocol version is bumped only when RPC request/response schemas change in backward-incompatible ways (new required fields, changed semantics). Adding optional fields does not require a version bump.

### Use `db_open_fast()` in fallback paths (S)

When the server isn't available and the CLI falls back to in-process execution, use `db_open_fast()` instead of `db_open()` for commands that don't need migrations. `db_open_fast()` already exists (`db.c:781-818`) and skips migrations when the schema is initialized (~3-10ms vs 50-150ms).

### Changes

| File | Change |
|------|--------|
| `src/cli_client.c` | Add `cli_rpc_for_command()` route table, `cli_rpc_forward()` generic forwarder |
| `src/main.c` | Add RPC routing before command table lookup (like hooks at line 256) |
| `src/cmd_agent_trace.c` | Replace `db_open(NULL)` with `db_open_fast(NULL)` in `cmd_delegate()` (lines 351, 372) |
| `src/cmd_memory.c` | Replace `db_open()` with `db_open_fast()` |
| `src/cmd_rules.c` | Replace `db_open()` with `db_open_fast()` |

## Acceptance Criteria

- [ ] `time aimee memory search test` completes in <10ms when server is running (measured via `AIMEE_SOCK`)
- [ ] `time aimee rules generate` completes in <10ms when server is running
- [ ] `time aimee delegate analyst "test" --background --json` returns task_id in <10ms when server is running
- [ ] All commands fall back to in-process execution when no server is available
- [ ] CLI falls back to in-process execution when server returns protocol version mismatch
- [ ] `server.info` response includes `protocol_version` field
- [ ] Fallback path uses `db_open_fast()` — no migration checks on warm DB
- [ ] All existing integration tests pass without modification
- [ ] No regression in `make test` suite

## Owner and Effort

- **Owner:** JBailes
- **Effort:** M (RPC routing is M due to arg marshaling per command; `db_open_fast` swap is S)
- **Dependencies:** Benefits from the on-demand server proposal (ensuring a server is always available), but works with existing `cli_ensure_server()` auto-start.

## Rollout and Rollback

- **Rollout:** Direct code changes, no feature flags. RPC routing and `db_open_fast` are separate commits.
- **Rollback:** `git revert` of either commit. No migrations or state changes.
- **Blast radius:** Affects all CLI subcommand invocations when a server is running. If RPC forwarding is broken, commands fall back to in-process. `db_open_fast()` skips migrations — if a new migration is pending after upgrade, the first in-process command runs against the old schema until the server (which uses full `db_open()`) runs migrations.

## Test Plan

- [ ] Unit tests: `cli_rpc_for_command()` returns correct method for each routed command, NULL for unrouted
- [ ] Unit tests: `cli_rpc_forward()` correctly marshals argv to JSON and unmarshals response
- [ ] Integration tests: `aimee memory store` / `search` / `list` / `get` work through server RPC
- [ ] Integration tests: `aimee delegate --background` returns task_id via server RPC
- [ ] Integration tests: all routed commands fall back to in-process when `AIMEE_NO_AUTOSTART=1`
- [ ] Performance test: 100 sequential `aimee memory search` calls complete in <2s total
- [ ] Failure injection: server dies mid-request — CLI gets error, not hang
- [ ] Integration test: CLI with protocol_version=2 connecting to server supporting only version=1 falls back to in-process

## Operational Impact

- **Metrics:** No new metrics.
- **Logging:** `cli_rpc_forward()` logs to stderr on connection failure before fallback.
- **Alerts:** None.
- **Disk/CPU/Memory:** Reduces per-invocation memory (~SQLite buffers not allocated in CLI process). Shifts CPU from N CLI processes to 1 server process.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Route CLI through server RPCs | P0 | M | 70-250ms → 2-3ms per delegate call |
| Use `db_open_fast()` in fallback | P1 | S | 30-100ms savings when no server |

## Trade-offs

**Server dependency for fast path:** Delegates get <10ms only when a server is running. Without a server, they fall back to in-process (70-250ms with `db_open_fast`, vs 70-250ms today with `db_open`). For delegates spawned within a session, `AIMEE_SOCK` is inherited — they always hit the warm server.

**Socket trust:** Before connecting to `AIMEE_SOCK` or the well-known socket, `cli_rpc_forward()` must validate socket ownership: `stat()` the socket file and verify `st_uid == getuid()` and permissions are `0600`. This prevents connecting to an attacker-controlled socket on shared hosts. If the check fails, fall back to in-process execution. (See also: on-demand-server proposal for socket creation permissions.)

**Arg marshaling complexity:** Each CLI command has its own argument structure that must be translated to JSON for the RPC. This is boilerplate but must be maintained in sync with the server handlers. Mitigated by keeping the route table declarative and testing each route.

**`db_open_fast()` skips migrations:** If a new migration is pending (after upgrade), the first command to use `db_open_fast()` will run against the old schema. Acceptable because the server runs full `db_open()` on startup and handles migrations there. `db_open_fast()` falls back to `db_open()` if the schema isn't initialized at all.
