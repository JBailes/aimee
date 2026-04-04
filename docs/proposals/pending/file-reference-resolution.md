# Proposal: File Reference Resolution in Delegate Prompts

## Problem

When dispatching delegates, the orchestrator often describes tasks that reference specific files ("update the handler in src/mcp_tools.c", "follow the pattern in src/guardrails.c"). The delegate must then spend a tool call reading each referenced file before it can start working. This wastes a round-trip and context on every delegation.

Evidence: oh-my-openagent's `file-reference-resolver.ts` (`src/shared/file-reference-resolver.ts`) scans prompt text for `@file_path` references and replaces them with the file's actual content. It includes safety checks: files must be within the project root, and recursion is limited to depth 3.

## Goals

- Support `@path/to/file` syntax in delegate prompts
- Automatically inline referenced file content when dispatching
- Safety: only resolve files within the project directory
- Limit: max 3 file references per prompt, max 10KB per file

## Approach

When `agent_coord.c` prepares a delegate prompt, scan for `@path` patterns. For each match, resolve the path relative to the project root, read the file, and replace the reference with the content. Reject paths outside the project or that exceed size limits.

### Changes

| File | Change |
|------|--------|
| `src/agent_coord.c` | Add `resolve_file_references()` to delegate prompt preparation |
| `src/headers/agent.h` | Add file reference resolution config (max files, max size) |

## Acceptance Criteria

- [ ] `@src/foo.c` in a delegate prompt is replaced with the file's content
- [ ] Paths outside the project root are rejected with `[file outside project: ...]`
- [ ] Non-existent files produce `[file not found: ...]`
- [ ] Max 3 references per prompt (excess are left as-is)
- [ ] Files >10KB are truncated with `[TRUNCATED]`

## Owner and Effort

- **Owner:** aimee
- **Effort:** M (1–2 days)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Compile-time inclusion; `@path` syntax is opt-in by usage
- **Rollback:** Remove resolution; `@path` references pass through as literal text
- **Blast radius:** Only affects delegate prompt text

## Test Plan

- [ ] Unit test: valid file reference is inlined
- [ ] Unit test: path outside project is rejected
- [ ] Unit test: non-existent file produces clear error
- [ ] Unit test: >3 references — first 3 resolved, rest left as-is
- [ ] Unit test: large file is truncated

## Operational Impact

- **Metrics:** File references resolved per delegation
- **Logging:** Log resolutions at debug level
- **Disk/CPU/Memory:** File reads during delegation; bounded by limits

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| File Reference Resolution | P3 | M | Low-Medium — saves one round-trip per delegation |

## Trade-offs

Alternative: require the orchestrator to Read files and paste content into the prompt manually. Current behavior — works but wasteful and error-prone (orchestrator may paste stale content).

Inspiration: oh-my-openagent `src/shared/file-reference-resolver.ts`
