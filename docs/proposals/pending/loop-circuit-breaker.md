# Proposal: Loop / Circuit Breaker Detection

## Problem

Delegate agents sometimes enter infinite loops — repeating the same tool call with identical arguments. This burns tokens, wastes compute, and produces no useful work. Common patterns: repeatedly reading the same file, retrying the same failed bash command, or calling grep with the same query. Without intervention, a stuck delegate can exhaust its entire context window on duplicate calls.

Evidence: oh-my-openagent implements a circuit breaker in `src/features/background-agent/loop-detector.ts`. It hashes each tool call (tool name + sorted JSON of arguments) and tracks consecutive identical signatures. When the count exceeds a threshold, it trips the breaker and stops the agent.

## Goals

- Detect when a delegate makes N consecutive identical tool calls
- Interrupt the loop with a corrective message or abort the session
- Configurable threshold (default: 3 consecutive identical calls)
- Zero false positives on legitimate repeated calls (e.g., polling)

## Approach

Add loop detection to the agent tool execution pipeline. Before dispatching a tool call, compute a signature from the tool name and arguments. If the signature matches the previous call, increment a counter. When the counter exceeds the threshold, inject a corrective system message or abort.

### Signature computation

```c
// Hash: tool_name + sorted JSON of arguments
uint32_t sig = xxhash32(tool_name, strlen(tool_name), 0);
sig = xxhash32(sorted_args_json, args_len, sig);
```

### Changes

| File | Change |
|------|--------|
| `src/agent_tools.c` | Add signature tracking and loop detection before tool dispatch |
| `src/agent_eval.c` | Check circuit breaker state; inject corrective message or abort |
| `src/headers/agent.h` | Add `loop_detector` struct to session state |

## Acceptance Criteria

- [ ] 3 consecutive identical tool calls triggers the breaker
- [ ] Breaker injects a "you're looping — re-evaluate your approach" message
- [ ] After 5 consecutive identical calls, the session is aborted
- [ ] Different tool calls reset the counter
- [ ] Threshold is configurable via project config

## Owner and Effort

- **Owner:** aimee
- **Effort:** S–M (1–2 days)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Compile-time inclusion, always active for delegate sessions
- **Rollback:** Remove the check; delegates run without loop detection as before
- **Blast radius:** Only affects delegate tool execution; orchestrator sessions unaffected

## Test Plan

- [ ] Unit test: 3 identical calls triggers warning injection
- [ ] Unit test: 5 identical calls triggers abort
- [ ] Unit test: non-identical calls reset counter
- [ ] Unit test: signature hashing is deterministic across argument order
- [ ] Integration test: delegate with intentionally looping prompt, verify abort

## Operational Impact

- **Metrics:** Count of circuit breaker triggers per session, per tool
- **Logging:** Log warning at trigger, error at abort
- **Disk/CPU/Memory:** Negligible — one hash comparison per tool call

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Loop Circuit Breaker | P0 | S–M | High — prevents runaway delegate token burn |

## Trade-offs

Alternative: track a sliding window of recent calls instead of just consecutive. More robust but adds complexity for marginal gain — truly stuck agents repeat the exact same call, not interleaved patterns. Consecutive detection covers the vast majority of cases.

Inspiration: oh-my-openagent `src/features/background-agent/loop-detector.ts`
