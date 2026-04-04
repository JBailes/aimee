# Proposal: Hierarchical Context and Rule Discovery

## Problem

Several pending proposals describe the same underlying gap: aimee lacks a coherent way to discover and inject local instructions.

- ancestor-chain rule discovery is incomplete
- per-directory context files are not injected
- rule/convention files are not surfaced near the edited file

These should be one context-discovery proposal. The agent should not have to separately learn project root rules, nearest directory context, and convention files through unrelated mechanisms.

## Goals

- Discover instruction and convention files from the current directory up through the project root.
- Prioritize closer, more relevant files over distant ones.
- Deduplicate repeated content and enforce budget caps.
- Inject relevant local context once per directory/session rather than on every read/edit.
- Use the same logic across CLI, webchat, and delegates.

## Approach

Build one hierarchical context-discovery subsystem covering three file classes:

1. explicit aimee rules
2. nearest per-directory context files
3. nearby convention files

### Discovery Order

Walk from the target file directory toward the project root and consider:

- `.aimee-rules`
- `.aimee/context.md`
- `AGENTS.md`
- `.aimee/rules.md`
- `CONTRIBUTING.md`
- `.editorconfig`
- configured custom rule/context paths

Closer files win, but ancestor-chain discovery allows cross-cutting rules to still apply when budget permits.

### Injection Policy

- inject once per `(session, directory, file-content-hash)` style key
- deduplicate identical content by hash
- apply per-file and total context budgets
- prefer explicit context files over generic convention files when budgets are tight

### Changes

| File | Change |
|------|--------|
| `src/rules.c` | Ancestor-chain discovery, deduplication, and budget caps |
| `src/mcp_tools.c` | Local context/rule injection for Read/Edit/Write flows |
| `src/agent_context.c` | Use unified discovery output when building prompts |
| `src/config.c` | Parse discovery budgets and custom rule/context paths |
| `src/server_session.c` | Track injected directory/rule state per session |

## Acceptance Criteria

- [ ] Nested directory rules and context files are discovered from the target path to the project root.
- [ ] Duplicate content is loaded only once.
- [ ] Per-file and total budget caps prevent prompt bloat.
- [ ] Nearest context file wins when multiple candidates exist.
- [ ] Convention/rule injection is cached per session/directory to avoid repetition.
- [ ] CLI, webchat, and delegates use the same discovery logic.

## Owner and Effort

- **Owner:** aimee
- **Effort:** M
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Start with ancestor discovery and budgets, then add targeted tool-output injection.
- **Rollback:** Reduce discovery scope back to project-root-only rules.
- **Blast radius:** Unexpected ancestor rules could influence prompts, so discovered files should be visible in logs/debug output.

## Test Plan

- [ ] Unit tests: ancestor walk, nearest-file precedence, deduplication, and budget truncation
- [ ] Unit tests: rule/context injection caching
- [ ] Integration tests: nested directory trees with mixed context/rule files
- [ ] Manual verification: place rules at multiple levels and confirm the final injected set is sensible

## Operational Impact

- **Metrics:** `context_files_discovered`, `context_injections_total`, `context_budget_truncations_total`
- **Logging:** INFO/DEBUG on discovered files and budget decisions
- **Alerts:** None
- **Disk/CPU/Memory:** Small directory-walk overhead with session caching

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Ancestor-chain discovery | P1 | S | High |
| Nearest-directory context injection | P1 | S | High |
| Budget caps and deduplication | P1 | S | High |

## Trade-offs

- **Why merge these proposals?** Rule discovery and local context injection are one input-selection problem.
- **Why not inject everything always?** Context budgets matter; relevance ranking is more important than completeness.
- **Why include convention files like `.editorconfig` and `CONTRIBUTING.md`?** They often encode the exact conventions agents otherwise miss.
