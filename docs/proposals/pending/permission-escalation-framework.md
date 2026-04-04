# Proposal: Hierarchical Permission Escalation and Scoped Tool Permissions

## Problem

There are three overlapping permission proposals in the pending set:

- hierarchical escalation levels
- per-tool tiering and delegate allowlists
- fine-grained scoped rules by file, directory, command, or URL

These are one authorization system, not three. Aimee needs tool tiers, scoped rules, and escalation to work together.

The claw-code project implements a hierarchical permission model in `runtime/permissions.rs`:
- **Permission levels**: ReadOnly < WorkspaceWrite < DangerFullAccess < Prompt < Allow
- **Per-tool requirements**: Each tool declares its minimum permission level
- **Escalation**: When a tool requires a higher level than the session's current mode, the user is prompted
- **Prompter trait**: Pluggable approval mechanism (interactive prompt, auto-approve, logging)

Aimee's guardrails block specific patterns (e.g., `rm -rf`, `git push --force`) but can't express "this session should freely read and write files but ask before running shell commands" or "this delegate can do anything in the workspace but nothing outside it."

## Goals

- Tools declare required permission levels.
- Sessions have a default auto-approve ceiling.
- Scoped rules can narrow or widen permissions by file path, directory, command pattern, or URL.
- Delegates can be launched with restricted tool sets.
- Interactive sessions can escalate; headless sessions deny above-policy actions.
- Existing guardrails remain as an extra safety layer.

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

### 2. Tool Registration and Filtering

Each MCP tool and built-in tool declares its required level in the tool manifest:

```c
struct tool_permission {
    const char *tool_name;      /* e.g., "mcp__aimee__git_push" */
    enum permission_level level; /* PERM_DANGEROUS */
};
```

Support alias-normalized allowlists such as `--tools read,grep,glob`.

### 3. Session Policy and Scoped Rules

Sessions carry a `max_auto_approve` level. Operations at or below this level proceed without prompting. Operations above it trigger the escalation handler.

Default levels by context:
- **Interactive CLI**: `PERM_EXECUTE` (prompts for dangerous)
- **Delegate (background)**: `PERM_WRITE` (prompts for execute and dangerous)
- **Autonomous mode**: `PERM_DANGEROUS` (auto-approve everything, for `--autonomous` flag)

Add scoped rule evaluation ahead of the final allow/deny decision:

- file pattern rules
- directory subtree rules
- command regex/glob rules
- URL/domain rules

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
| `src/agent_policy.c` | Add unified permission check, tool-level mapping, and scoped-rule evaluation |
| `src/server_session.c` | Add session permission state and escalation context |
| `src/mcp_tools.c` | Annotate tools with required levels and apply filtered tool registration |
| `src/config.c` | Parse default levels, autonomous mode, and scoped permission rules |
| `src/headers/agent_policy.h` | Permission enums, rule structs, and escalation API |
| `src/cmd_agent.c` | Add delegate `--tools` allowlist support |

## Acceptance Criteria

- [ ] `aimee delegate code "..."` at default level can read/write files but prompts before running bash
- [ ] `aimee delegate execute --autonomous "..."` auto-approves all operations
- [ ] A tool requiring `PERM_DANGEROUS` in a `PERM_WRITE` session is denied in headless mode
- [ ] Scoped rules can restrict writes to directories/files and restrict web or shell usage by pattern
- [ ] `--tools read,grep,glob` is alias-normalized and enforced
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
| Unified permission framework | P2 | M | High — enables safe autonomous operation and fine-grained control |

## Trade-offs

- **Why merge the permission proposals?** Tiering, escalation, scoped rules, and allowlisted tools are all parts of the same decision path.
- **ACL-based permissions** (per-user, per-role) were considered but aimee is single-user. Session-level granularity is sufficient.
- **Capability tokens** (grant specific capabilities per-session) would be more flexible but more complex. Level-based rules plus scopes cover the common cases.
