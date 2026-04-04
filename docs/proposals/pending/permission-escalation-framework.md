# Proposal: Hierarchical Permission Escalation Framework

## Problem

Aimee's current tool authorization is binary: `pre_tool_check` in `agent_policy.c` either allows or blocks a tool call based on pattern matching. There is no concept of permission levels, no escalation flow, and no way to grant blanket permission for safe operations while requiring approval for dangerous ones.

The claw-code project implements a hierarchical permission model in `runtime/permissions.rs`:
- **Permission levels**: ReadOnly < WorkspaceWrite < DangerFullAccess < Prompt < Allow
- **Per-tool requirements**: Each tool declares its minimum permission level
- **Escalation**: When a tool requires a higher level than the session's current mode, the user is prompted
- **Prompter trait**: Pluggable approval mechanism (interactive prompt, auto-approve, logging)

Aimee's guardrails block specific patterns (e.g., `rm -rf`, `git push --force`) but can't express "this session should freely read and write files but ask before running shell commands" or "this delegate can do anything in the workspace but nothing outside it."

## Goals

- Tools declare a required permission level (read, write, execute, dangerous).
- Sessions start at a configurable permission level.
- Operations exceeding the session's level trigger an escalation prompt (interactive) or denial (headless).
- Permission grants can be scoped: per-tool, per-session, or persistent.
- Existing `pre_tool_check` pattern matching is retained as an additional layer.

## Approach

### 1. Permission Levels

```c
enum permission_level {
    PERM_READ,          /* file reads, index queries, memory search */
    PERM_WRITE,         /* file edits, git add/commit */
    PERM_EXECUTE,       /* bash commands, process spawning */
    PERM_DANGEROUS,     /* git push, file deletion, network access */
};
```

### 2. Tool Registration

Each MCP tool and built-in tool declares its required level in the tool manifest:

```c
struct tool_permission {
    const char *tool_name;      /* e.g., "mcp__aimee__git_push" */
    enum permission_level level; /* PERM_DANGEROUS */
};
```

### 3. Session Policy

Sessions carry a `max_auto_approve` level. Operations at or below this level proceed without prompting. Operations above it trigger the escalation handler.

Default levels by context:
- **Interactive CLI**: `PERM_EXECUTE` (prompts for dangerous)
- **Delegate (background)**: `PERM_WRITE` (prompts for execute and dangerous)
- **Autonomous mode**: `PERM_DANGEROUS` (auto-approve everything, for `--autonomous` flag)

### 4. Escalation Handler

```c
typedef int (*permission_prompter_fn)(const char *tool_name,
                                       enum permission_level required,
                                       enum permission_level current,
                                       char *reason, size_t reason_sz);
```

Implementations:
- **Interactive**: Print prompt, wait for y/n
- **Headless**: Deny with logged reason
- **Auto-approve**: Always grant (for autonomous mode)
- **Recording**: Log all decisions for audit

### Changes

| File | Change |
|------|--------|
| `src/agent_policy.c` | Add `permission_check()` before `pre_tool_check()`; define tool→level mapping |
| `src/server_session.c` | Add `max_auto_approve` to session state; pass to policy checks |
| `src/mcp_tools.c` | Annotate each MCP tool with its required permission level |
| `src/config.c` | Add `permissions.default_level` and `permissions.autonomous` config keys |
| `src/headers/agent_policy.h` | Define `permission_level` enum, `permission_prompter_fn` typedef |

## Acceptance Criteria

- [ ] `aimee delegate code "..."` at default level can read/write files but prompts before running bash
- [ ] `aimee delegate execute --autonomous "..."` auto-approves all operations
- [ ] A tool requiring `PERM_DANGEROUS` in a `PERM_WRITE` session is denied in headless mode
- [ ] `aimee config set permissions.default_level execute` changes the default
- [ ] Existing `pre_tool_check` blocklist patterns still block even if permission level would allow
- [ ] Permission decisions are logged for audit

## Owner and Effort

- **Owner:** aimee maintainer
- **Effort:** M (the framework is simple; main work is annotating all tools with correct levels)
- **Dependencies:** None (complements sandbox-tool-execution proposal but independent)

## Rollout and Rollback

- **Rollout:** Default behavior matches current: execute-level auto-approve for interactive, write-level for delegates. No behavior change unless configured.
- **Rollback:** Remove permission checks from `agent_policy.c`; fall back to pattern-only guardrails.
- **Blast radius:** Could block previously-allowed operations if tool levels are set too high. Mitigated by conservative defaults.

## Test Plan

- [ ] Unit tests: `permission_check()` with each level combination
- [ ] Integration tests: delegate session at each permission level attempting operations above and below
- [ ] Failure injection: prompter returns error; verify session degrades to deny-all
- [ ] Manual verification: interactive session prompts for dangerous operation, accepts y/n

## Operational Impact

- **Metrics:** `permission.granted`, `permission.denied`, `permission.escalated` counters per level
- **Logging:** INFO on grant, WARN on deny, DEBUG on auto-approve
- **Alerts:** None
- **Disk/CPU/Memory:** Negligible — a lookup table check per tool call

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Permission framework | P2 | M | High — enables safe autonomous operation and fine-grained control |

## Trade-offs

- **ACL-based permissions** (per-user, per-role) were considered but aimee is single-user. Session-level granularity is sufficient.
- **Capability tokens** (grant specific capabilities per-session) would be more flexible but more complex. Level-based is simpler and covers the common cases.
- **Retroactive revocation** (downgrading mid-session) was considered but adds complexity. Sessions keep their initial level for simplicity.
