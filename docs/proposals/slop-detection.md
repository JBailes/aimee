# Proposal: AI slop detection in guardrails

## Problem

AI coding agents frequently generate "slop" -- low-value comments, redundant docstrings, attribution markers, and filler text that degrades code quality over time. Examples:

- `// This function handles the login logic` above `function handleLogin()`
- `// TODO: Add error handling here` with no actual TODO context
- `# Created by AI assistant` or `// Generated with Claude Code`
- Restating the function signature in a docstring: `"""Gets the user by ID. Args: user_id: The user ID."""`
- `// eslint-disable-next-line` added to suppress warnings rather than fixing them

oh-my-openagent includes a "Comment Checker" that detects AI-generated code comments during tool execution and blocks or warns about them. aimee's guardrails (`guardrails.c`) already intercept tool calls for path classification and anti-pattern detection, but don't check the content of writes for slop.

## Goals

- Detect common AI slop patterns in file writes (Edit, Write tool calls) during hooks.
- Warn on stderr (like existing anti-pattern detection) rather than hard-block.
- Track slop frequency in metrics for feedback to the user.
- Apply the same detection in webchat when the agent writes files.

## Approach

### Detection rules

Add a slop detector to `guardrails.c` that inspects the content of write operations. Detection is line-based pattern matching:

| Pattern | Category | Example |
|---------|----------|---------|
| Comment restates function name | Redundant | `// handleLogin: handles login` |
| Attribution markers | Attribution | `Generated with`, `Co-Authored-By`, `Created by AI` |
| Empty TODO/FIXME | Hollow TODO | `// TODO:` or `// FIXME` with no description |
| Docstring mirrors signature | Redundant | `"""Gets user by id. Args: id: the id"""` |
| Lint suppression without context | Suppression | `// eslint-disable-next-line` with no explanation |
| Obvious type annotations in comments | Redundant | `// string`, `// returns boolean` |

### Implementation

```c
typedef struct {
    const char *pattern;     /* regex or substring */
    const char *category;    /* redundant, attribution, hollow_todo, suppression */
    int severity;            /* 0=info, 1=warn */
} slop_rule_t;

int guardrails_check_slop(const char *content, const char *filename,
                          char *warnings, size_t warnings_cap);
```

Called from `guardrails_pre_tool()` when the tool is `Edit`, `Write`, or `write_file`. Only inspects the new/changed content, not the entire file.

### Metrics

Add a `slop_detections` counter to the session metrics, broken down by category. Surfaced in `aimee session stats` and the dashboard.

### Webchat parity

The webchat agent's tool execution path already goes through the same guardrails infrastructure. Slop detections are included in the SSE stream as warnings, rendered in the chat UI as yellow notices.

### Changes

| File | Change |
|------|--------|
| `src/guardrails.c` | Add `guardrails_check_slop()` with pattern table and line scanner |
| `src/headers/guardrails.h` | Declare slop check API |
| `src/cmd_hooks.c` | Call slop check in pre-tool hook for write operations |
| `src/agent_tools.c` | Call slop check before delegate write_file execution |
| `src/webchat.c` | Include slop warnings in SSE tool-result events |
| `src/dashboard.c` | Add slop detection count to metrics card |

## Acceptance Criteria

- [ ] Write operations containing attribution markers produce a warning on stderr
- [ ] Write operations with redundant comments (restating function name) produce a warning
- [ ] Warnings include the line number, category, and the offending line
- [ ] `aimee --json hooks pre` includes slop warnings in the JSON output
- [ ] Slop detection does not add more than 1ms to pre-tool hook latency
- [ ] Delegate `write_file` tool also checks for slop

## Owner and Effort

- **Owner:** aimee contributor
- **Effort:** S (1-2 days)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Active immediately. Warn-only by default (no hard blocks).
- **Rollback:** Revert commit. No state changes.
- **Blast radius:** Worst case: false-positive warnings on legitimate comments. Warn-only means no workflow disruption.

## Test Plan

- [ ] Unit tests: each slop pattern with positive and negative examples
- [ ] Unit test: verify latency stays under 1ms for typical file sizes
- [ ] Integration test: write a file with attribution via hook, verify warning appears
- [ ] Manual verification: trigger each category and confirm warning message is clear

## Operational Impact

- **Metrics:** New `slop_detections` counter by category.
- **Logging:** Slop warnings at WARN level.
- **Disk/CPU/Memory:** Negligible. Line-based pattern matching on write content only.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Slop detection | P2 | S | Medium -- improves code quality, low effort |

## Trade-offs

- **Alternative: hard-block slop.** Rejected as too aggressive. Warn-only lets users override when the comment is intentional.
- **Alternative: AST-based detection.** More accurate but dramatically more complex. Line-based patterns catch 90% of cases with 10% of the effort.
- **Limitation:** False positives are possible. The pattern list should be conservative and tunable via config.
