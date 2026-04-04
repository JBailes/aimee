# Proposal: Per-Directory Context Injection

## Problem

Aimee's context injection is project-level only (CLAUDE.md, `.aimee/project.yaml`). Large projects have per-module conventions that aren't captured at the project root: the `src/server_*.c` files follow different patterns than `src/cli_*.c`; the `tests/` directory has its own setup requirements. Delegates working in a specific directory lack local knowledge and make mistakes that a per-directory context file would prevent.

Evidence: oh-my-openagent implements two complementary hooks: `directory-agents-injector` and `directory-readme-injector` (`src/hooks/directory-agents-injector/`, `src/hooks/directory-readme-injector/`). When an agent reads a file, the hook finds the nearest `AGENTS.md` in the directory hierarchy and injects its content into the tool output. This gives agents per-directory knowledge automatically.

## Goals

- Support per-directory context files (e.g., `AGENTS.md` or `.aimee/context.md`)
- Auto-inject the nearest context file when a delegate reads/edits files in that directory
- Walk up the directory tree to find the nearest context file (closest wins)
- Inject once per directory per session (not on every file read)

## Approach

When the MCP Read or Edit tool returns, check if the file's directory (or any parent up to the project root) contains a context file. If found and not already injected for this session+directory combination, append the context to the tool output.

### Context file discovery

```
src/server_mcp.c  →  look for:
  src/AGENTS.md
  src/.aimee/context.md
  AGENTS.md
  .aimee/context.md
```

First match wins. Cache per (session, directory) to avoid re-injection.

### Changes

| File | Change |
|------|--------|
| `src/mcp_tools.c` | Add context file discovery and injection on Read/Edit |
| `src/server_session.c` | Track injected directories per session |

## Acceptance Criteria

- [ ] `AGENTS.md` in `src/` is injected when reading `src/foo.c`
- [ ] Injection happens once per directory per session
- [ ] Nearest context file wins (child directory overrides parent)
- [ ] Missing context files cause no error or injection
- [ ] Context injection is appended to tool output, not prepended

## Owner and Effort

- **Owner:** aimee
- **Effort:** M (1–2 days)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Compile-time inclusion; active when context files exist
- **Rollback:** Remove injection; Read/Edit return raw output as before
- **Blast radius:** Adds text to tool output when context files exist; no change otherwise

## Test Plan

- [ ] Unit test: context file in same directory is found and injected
- [ ] Unit test: context file in parent directory is found when child has none
- [ ] Unit test: second read in same directory does not re-inject
- [ ] Unit test: no context file produces no injection
- [ ] Integration test: delegate reads file near AGENTS.md, verify context appears

## Operational Impact

- **Metrics:** Context injections per session
- **Logging:** Log injection at debug level with directory and file path
- **Disk/CPU/Memory:** One directory stat walk per new directory; cached after first check

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Per-Directory Context | P2 | M | Medium — gives delegates local module knowledge |

## Trade-offs

Alternative: require delegates to read context files manually. Unreliable — delegates don't know to look for them. Auto-injection ensures context is always available.

Alternative: embed all per-directory rules in the project-level config. Doesn't scale — a project with 20 modules would have an enormous config file that wastes context on every session.

Inspiration: oh-my-openagent `src/hooks/directory-agents-injector/`, `src/hooks/directory-readme-injector/`
