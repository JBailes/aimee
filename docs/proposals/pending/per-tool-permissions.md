# Proposal: Per-Tool Tiered Permissions with Escalation

## Problem

Aimee's guardrail system (`guardrails.c`) operates as a binary allow/deny gate based on command patterns. It cannot express:

1. **Tiered risk levels** — reading a file is less dangerous than executing bash. Today all tools are treated equally.
2. **Interactive escalation** — in interactive mode (CLI or webchat), a user should be asked "allow this Write?" rather than having it silently allowed.
3. **Per-agent restriction** — a "review" delegate should only have Read/Grep/Glob tools, not Bash/Write/Edit.

The `soongenwong/claudecode` repo at `rust/crates/runtime/src/permissions.rs` implements tiered permissions with escalation, and `rust/crates/tools/src/lib.rs` implements allowed-tools filtering with alias normalization.

## Goals

- Each tool has a declared risk tier (read, write, execute, danger).
- Delegates can be restricted: `aimee delegate review --tools read,grep,glob "Review PR"`.
- Interactive sessions (CLI and webchat) prompt for escalation when a tool exceeds the session's tier.
- Headless sessions deny above-tier tools with clear messages.
- Tool name aliases resolve correctly: `read` → `read_file`, `bash` → `shell_exec`.

## Approach

### Permission Tiers

| Tier | Tools |
|------|-------|
| READONLY | read_file, grep_search, glob_search, lsp_* |
| WORKSPACE_WRITE | edit_file, write_file |
| EXECUTE | shell_exec, delegate, mcp_tool_call |
| DANGER | git_push, git_reset, rm/sudo patterns |

### Allowed-Tools Filtering

When `--tools` is specified, normalize and validate:
```
Input:  "read, grep, Edit"
Normalize: "read_file", "grep_search", "edit_file"
Result: only those tools are registered for the agent
```

### Changes

| File | Change |
|------|--------|
| `src/permissions.c` (new) | Permission tier definitions, tool-to-tier mapping, escalation logic |
| `src/tool_filter.c` (new) | Allowed-tools parsing, alias normalization, validation |
| `src/agent_tools.c` | Check permissions before executing, filter tool definitions by allowed set |
| `src/mcp_tools.c` | Apply tool filter when registering tools |
| `src/cmd_agent.c` | Add `--tools` flag to delegate commands |
| `src/webchat.c` | Interactive escalation prompts for webchat sessions |

## Acceptance Criteria

- [ ] `aimee delegate review --tools read,grep,glob "task"` restricts delegate to read-only tools
- [ ] Tool aliases (`read` → `read_file`) resolve correctly
- [ ] Invalid tool names produce clear error messages listing valid names
- [ ] Interactive sessions prompt for escalation; headless sessions deny with explanation
- [ ] Works in both CLI and webchat

## Owner and Effort

- **Owner:** delegate (code)
- **Effort:** M (2-3 days)
- **Dependencies:** Benefits from plugin-system proposal (plugin tools need tier declarations)

## Rollout and Rollback

- **Rollout:** Default tier is DANGER (everything allowed). `--tools` is opt-in.
- **Rollback:** Remove `--tools` flag handling.
- **Blast radius:** Over-restrictive filtering causes delegate failures. Clear error messages mitigate.

## Test Plan

- [ ] Unit tests: tier comparison, alias normalization, filter parsing
- [ ] Integration tests: delegate with restricted tools cannot use excluded tools
- [ ] Manual verification: run interactive session, trigger escalation prompt

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Tool-to-tier mapping | P2 | S | High — foundation |
| --tools flag with aliases | P2 | S | High — most common use case |
| Interactive escalation (CLI + webchat) | P3 | M | Medium |
| Headless tier enforcement | P2 | S | High — CI/automation safety |

## Trade-offs

- **Why not just use guardrails?** Guardrails are reactive pattern matching. Permissions are proactive tier enforcement. They complement each other.
- **Why include aliases?** Users will type `read` not `read_file`. Without aliases, every `--tools` invocation is frustrating.

## Source Reference

Implementation reference: `soongenwong/claudecode` at `rust/crates/runtime/src/permissions.rs` and `rust/crates/tools/src/lib.rs`.
