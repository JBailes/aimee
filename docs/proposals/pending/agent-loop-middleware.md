# Proposal: Agent Loop Middleware Pipeline

## Problem

Aimee's agent loop (`agent.c`) has several cross-cutting concerns hardcoded inline: turn counting, token budget checks, error detection, and guardrail enforcement. Adding new per-turn behaviors (context warnings, cost limits, auto-compaction triggers, read-only enforcement) requires modifying the core loop each time. This makes the loop harder to reason about, test, and extend.

Inspired by mistral-vibe's `MiddlewarePipeline`, which composes `ConversationMiddleware` objects that run `before_turn()` hooks and return actions (CONTINUE, STOP, COMPACT, INJECT_MESSAGE).

## Goals

- Agent loop behaviors are composable middleware functions, not inline code.
- New behaviors (budget warnings, cost limits, turn limits) can be added without touching the core loop.
- Middleware is configurable per agent profile — a "plan" agent gets different middleware than an "execute" agent.
- Works identically in CLI and webchat agent execution paths.

## Approach

Define a middleware interface: a function pointer that receives the current agent context (messages, turn count, token usage, cost, tool history) and returns an action enum plus an optional injected message.

```c
typedef enum {
    MW_CONTINUE,      /* proceed normally */
    MW_STOP,          /* halt the agent loop */
    MW_COMPACT,       /* trigger context compaction before next turn */
    MW_INJECT         /* inject a system message, then continue */
} mw_action_t;

typedef struct {
    mw_action_t action;
    char message[1024];  /* for MW_INJECT */
} mw_result_t;

typedef mw_result_t (*middleware_fn)(const agent_context_t *ctx, void *userdata);
```

A `middleware_pipeline` runs all registered middleware in order before each turn. On MW_STOP or MW_COMPACT, it short-circuits. MW_INJECT messages are concatenated.

### Built-in Middleware

| Middleware | Action | Description |
|-----------|--------|-------------|
| `mw_turn_limit` | MW_STOP | Halt after N turns (configurable) |
| `mw_cost_limit` | MW_STOP | Halt when session cost exceeds threshold |
| `mw_context_warning` | MW_INJECT | Warn when context usage hits 50%/75% |
| `mw_auto_compact` | MW_COMPACT | Trigger compaction at 80% context usage |
| `mw_readonly` | MW_STOP | Block file-writing tools in plan/chat profiles |
| `mw_stall_detect` | MW_INJECT | Warn when last N tool calls all errored |

### Changes

| File | Change |
|------|--------|
| `src/headers/middleware.h` | New: middleware types, pipeline API |
| `src/middleware.c` | New: pipeline run, built-in middleware implementations |
| `src/agent.c` | Replace inline checks with `middleware_pipeline_run()` call |
| `src/agent_config.c` | Load middleware config per agent profile |
| `src/webchat.c` | Use same middleware pipeline for webchat agent execution |

## Acceptance Criteria

- [ ] Agent loop calls `middleware_pipeline_run()` before each turn
- [ ] Turn-limit middleware stops the loop at the configured max
- [ ] Context-warning middleware injects a warning at 50% usage
- [ ] Auto-compact middleware triggers compaction at 80% usage
- [ ] Middleware stack is configurable per agent profile in config
- [ ] Webchat agent loop uses the same middleware pipeline as CLI
- [ ] Adding a new middleware requires only implementing the function and registering it — no core loop changes

## Owner and Effort

- **Owner:** aimee
- **Effort:** M (2-3 days)
- **Dependencies:** None (but complements session-compaction proposal)

## Rollout and Rollback

- **Rollout:** Refactor. Existing behavior is preserved — current inline checks become built-in middleware.
- **Rollback:** Revert commit. No persistent state changes.
- **Blast radius:** All agent execution paths. Thorough testing required.

## Test Plan

- [ ] Unit tests: each built-in middleware in isolation
- [ ] Integration tests: pipeline with multiple middleware, verify ordering and short-circuit
- [ ] Failure injection: middleware that panics/crashes — pipeline must not corrupt agent state
- [ ] Manual verification: run a long session, observe context warning and auto-compact triggers

## Operational Impact

- **Metrics:** `middleware_triggered{name=...}` counter per middleware
- **Logging:** DEBUG for each middleware evaluation, INFO for non-CONTINUE actions
- **Alerts:** None
- **Disk/CPU/Memory:** Negligible — middleware runs once per turn, O(N) where N is middleware count (typically <10)

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Agent Loop Middleware Pipeline | P2 | M | High — architectural improvement enabling many future features |

## Trade-offs

**Alternative: Hook-based system (like the existing notification-hooks proposal).** Hooks run external commands; middleware runs in-process with access to agent state. These are complementary, not competing — hooks are for external notifications, middleware is for loop control.

**Alternative: Lua/script-based middleware.** More flexible but adds a dependency and complexity. C function pointers are sufficient for the expected middleware set and avoid the embedding overhead.

**Known limitation:** Middleware order matters (e.g., cost-limit should run before context-warning). The pipeline must document and enforce ordering conventions.
