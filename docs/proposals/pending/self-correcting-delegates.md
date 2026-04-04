# Proposal: Self-correcting delegate loops

## Problem

aimee delegates currently execute a task and return a result -- success or failure. If the result is partial or incorrect, the caller must manually re-delegate or adjust. There's no built-in mechanism for a delegate to evaluate its own output and retry until the task is fully complete.

oh-my-openagent's "Ralph Loop" runs a self-referential execution cycle: the agent evaluates its progress, identifies gaps, and continues until it determines the task is 100% complete. This eliminates the pattern where a delegate returns "done" but the work is actually 80% finished.

aimee already has `--retry N` on delegates for transient failures (API errors, timeouts), but this is blind retry -- it doesn't evaluate the quality of the result before deciding to retry.

## Goals

- Delegates can run in a self-correcting loop that evaluates output quality and continues if incomplete.
- The loop has configurable exit conditions: max iterations, completion threshold, verification command.
- The webchat shows loop progress (iteration count, completion estimate) in real time.

## Approach

### New flag: `--loop`

```
aimee delegate code --loop "Implement user authentication with tests"
aimee delegate code --loop --max-iter 5 --verify "make test" "Fix the failing CI"
```

`--loop` enables self-correcting mode. After each delegate execution:

1. **Evaluate**: Ask the delegate to self-assess completion (0-100%) and list remaining gaps.
2. **Verify** (optional): Run a verification command (`--verify CMD`). If it fails, feed the error output into the next iteration.
3. **Decide**: If completion >= threshold (default 95%) AND verify passes, stop. Otherwise, feed the self-assessment and verify output into a continuation prompt.
4. **Continue**: Run the next iteration with accumulated context.

### Implementation

The loop lives in `agent.c` (or a new `agent_loop.c`), wrapping the existing `agent_execute()` function:

```c
typedef struct {
    int max_iterations;        /* default 5 */
    int completion_threshold;  /* default 95 */
    char verify_cmd[1024];     /* optional verification command */
    int current_iteration;
    int last_completion;
    char *accumulated_context; /* growing context from prior iterations */
} agent_loop_t;

int agent_execute_loop(agent_loop_t *loop, agent_exec_ctx_t *ctx);
```

Each iteration appends to `accumulated_context` the previous result summary and any verify output, keeping the delegate informed of what's already been done and what still needs work.

### Completion evaluation

The self-assessment is done by appending a structured prompt to the delegate's final turn:

```
Rate your completion of the original task (0-100%). List any remaining gaps.
Respond in JSON: {"completion": N, "gaps": ["gap1", "gap2"]}
```

This is parsed from the delegate's response. If parsing fails, the loop treats it as incomplete and continues.

### Webchat parity

The webchat's delegate execution path uses the same `agent_execute_loop()`. SSE events include iteration progress:

```json
{"type": "loop_progress", "iteration": 2, "max": 5, "completion": 72, "gaps": ["tests not written"]}
```

The chat UI renders this as a progress indicator.

### Changes

| File | Change |
|------|--------|
| `src/agent.c` or `src/agent_loop.c` (new) | `agent_execute_loop()` with evaluate/verify/decide/continue cycle |
| `src/headers/agent_exec.h` | Declare `agent_loop_t` and `agent_execute_loop()` |
| `src/cmd_agent_trace.c` | Parse `--loop`, `--max-iter`, `--verify` flags in delegate command |
| `src/agent_tools.c` | Add verify command execution helper |
| `src/webchat.c` | Emit `loop_progress` SSE events during loop execution |
| `src/mcp_tools.c` | Add `loop` option to MCP delegate tool |

## Acceptance Criteria

- [ ] `aimee delegate code --loop "task"` runs up to 5 iterations, stopping when self-assessed completion >= 95%
- [ ] `--verify CMD` runs the command after each iteration; failure triggers continuation
- [ ] `--max-iter N` caps the iteration count
- [ ] Each iteration's delegation log records the iteration number and completion score
- [ ] `aimee --json delegate --loop` includes per-iteration results in output
- [ ] Webchat emits `loop_progress` SSE events and renders progress
- [ ] Total token usage across all iterations is tracked and reported

## Owner and Effort

- **Owner:** aimee contributor
- **Effort:** M (3-4 days)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Opt-in via `--loop` flag. Default delegate behavior unchanged.
- **Rollback:** Revert commit. No state changes.
- **Blast radius:** Only affects delegates invoked with `--loop`. Worst case: delegate loops to max iterations and stops.

## Test Plan

- [ ] Unit tests: loop state machine (evaluate, verify, decide, continue)
- [ ] Unit test: completion JSON parsing with malformed/missing responses
- [ ] Integration test: delegate with `--loop --verify "test -f output.txt"` creates file on second iteration
- [ ] Integration test: verify `--max-iter 1` stops after one iteration regardless of completion
- [ ] Manual verification: observe webchat loop progress rendering

## Operational Impact

- **Metrics:** New counters: `delegate_loop_iterations`, `delegate_loop_completions`, `delegate_loop_exhausted` (hit max-iter).
- **Logging:** Each iteration logged at INFO with completion score.
- **Disk/CPU/Memory:** Each iteration is a full delegate execution (tokens, time). Max-iter caps the cost.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Self-correcting loops | P2 | M | High -- dramatically improves delegate task completion quality |

## Trade-offs

- **Alternative: external orchestration.** The caller (Claude Code, etc.) could loop on delegate results. Rejected because aimee is better positioned to manage the loop state, accumulated context, and verification.
- **Alternative: fine-grained step execution.** Break tasks into planned steps and execute each. This is what `agent_plan.c` already does. The loop is complementary -- it handles the "almost done but not quite" case that plans can't anticipate.
- **Limitation:** Self-assessment is inherently unreliable. The verify command is the ground truth; self-assessment is a heuristic for when no verify command is provided.
