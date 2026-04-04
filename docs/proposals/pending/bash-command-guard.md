# Proposal: Bash Command Guard

## Problem

Agents frequently use bash commands (`cat`, `head`, `tail`, `grep`, `find`) for operations that have dedicated MCP tools (Read, Grep, Glob). Using bash for file reads bypasses features like line numbering, hash anchoring, and output truncation. Using bash for search bypasses the index. The results are also harder to parse and less structured.

Evidence: oh-my-openagent implements a bash file read guard (`src/hooks/bash-file-read-guard.ts`) that detects simple `cat`/`head`/`tail` commands and warns the agent to use the Read tool instead.

## Goals

- Detect bash commands that duplicate MCP tool functionality
- Warn the agent to use the appropriate structured tool instead
- Advisory only — don't block the bash command (it may be part of a pipeline)
- Cover the most common cases: file reads, grep, find

## Approach

In the MCP bash tool handler, before execution, pattern-match the command against known simple-tool-equivalent patterns. If matched, append a warning to the tool output suggesting the structured alternative.

### Patterns to detect

| Bash pattern | Suggested tool |
|---|---|
| `cat <file>` | Read |
| `head [-n N] <file>` | Read with limit |
| `tail [-n N] <file>` | Read with offset |
| `grep <pattern> <path>` | Grep |
| `find <path> -name <glob>` | Glob |

### Changes

| File | Change |
|------|--------|
| `src/mcp_tools.c` | Add pattern detection in bash tool handler; append warning to output |

## Acceptance Criteria

- [ ] Simple `cat file` commands trigger a "use Read instead" warning
- [ ] Simple `grep pattern path` commands trigger a "use Grep instead" warning
- [ ] Complex pipelines (`cat file | grep pattern | awk ...`) do NOT trigger
- [ ] Warning is appended to output, not blocking
- [ ] At least 5 common patterns are covered

## Owner and Effort

- **Owner:** aimee
- **Effort:** S (< 1 day)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Compile-time inclusion, always active
- **Rollback:** Remove pattern matching; bash runs without warnings as before
- **Blast radius:** Only affects bash tool output text; command still executes

## Test Plan

- [ ] Unit test: `cat foo.c` triggers warning
- [ ] Unit test: `cat foo.c | grep bar` does NOT trigger (pipeline)
- [ ] Unit test: `head -n 10 foo.c` triggers warning
- [ ] Unit test: `grep -r pattern src/` triggers warning
- [ ] Unit test: `find . -name "*.c"` triggers warning

## Operational Impact

- **Metrics:** Count of bash guard warnings per session
- **Logging:** Log at debug level
- **Disk/CPU/Memory:** Negligible — regex matching on command string

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Bash Command Guard | P1 | S | Medium — steers agents toward structured tools |

## Trade-offs

Alternative: block the bash command entirely when a structured tool exists. Too aggressive — sometimes the bash version is part of a larger pipeline or has flags the structured tool doesn't support.

Inspiration: oh-my-openagent `src/hooks/bash-file-read-guard.ts`
