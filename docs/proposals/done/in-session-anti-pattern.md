# Proposal: In-Session Anti-Pattern Checking

## Problem

Anti-pattern detection (`anti_pattern_check` in `memory_advanced.c`) and escalation (`anti_pattern_escalate`) only run at session cleanup (`cmd_hooks.c`). An agent that makes the same mistake 5 times within one session cannot course-correct until the next session. The working memory system (`working_memory.c`) provides session-scoped state but is not connected to anti-pattern matching. The pre-tool hook (`handle_hooks_pre` in `server.c`, `pre_tool_check` in `guardrails.c`) already runs before every tool call but does not check anti-patterns.

## Goals

- Anti-patterns are checked during agent execution, not just at cleanup.
- Repeated in-session failures trigger working-memory warnings.
- Agents see anti-pattern warnings in their context before making the same mistake again.
- No significant latency added to the tool-call hot path.

## Approach

1. In `pre_tool_check()` (`guardrails.c`), after existing checks pass, call `anti_pattern_check()` with the `tool_name` and `tool_input`.
2. If a match is found, write a working memory entry (`wm_set`) with `category='warning'` and TTL 30 minutes.
3. Agent context assembly already includes working memory, so the agent sees the warning on the next turn.
4. Track in-session hit counts in working memory (key: `ap_hits_<pattern_id>`, value: count). If count reaches 3, escalate the warning to a blocking message in the `pre_tool_check` response.
5. In-session counters reset naturally when the session ends (WM is session-scoped).

### Changes

| File | Change |
|------|--------|
| `src/guardrails.c` | Add `anti_pattern_check()` call in `pre_tool_check`; write WM warning on match; block on 3 repeated hits |
| `src/working_memory.c` | No changes needed; use existing `wm_set` API |

## Acceptance Criteria

- [ ] `pre_tool_check` calls `anti_pattern_check` for every tool invocation and returns a warning on first match.
- [ ] Matching anti-pattern writes a working memory entry with `category='warning'` and TTL of 30 minutes.
- [ ] After 3 matches of the same pattern in one session, `pre_tool_check` returns a blocking response.
- [ ] Non-matching tool calls see no added latency beyond ~2ms.
- [ ] Working memory hit counters reset when the session ends (no cross-session leakage).

## Owner and Effort

- **Owner:** aimee core
- **Effort:** M (medium)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Standard commit. No feature flag, migration, or config change required.
- **Rollback:** Revert commit. No database changes. Working memory entries expire naturally via TTL.
- **Blast radius:** Medium. False positive anti-pattern matches could block legitimate tool calls after 3 hits in a session.

## Test Plan

- [ ] Unit tests: anti-pattern matching inside `pre_tool_check` returns correct warning/block responses.
- [ ] Integration tests: trigger same anti-pattern 3 times, verify blocking on the third occurrence.
- [ ] Integration tests: non-matching tools are unaffected by the new check.
- [ ] Integration tests: verify WM entries are created with correct TTL and category.
- [ ] Failure injection: corrupt or missing anti-pattern database returns gracefully (no crash, no false block).
- [ ] Manual verification: run a session, trigger an anti-pattern twice, confirm warning appears in agent context.

## Operational Impact

- **Metrics:** New counter `anti_pattern_in_session_hits` tracking per-session matches.
- **Logging:** Log line when an anti-pattern warning or block occurs (level: INFO for warning, WARN for block).
- **Alerts:** None required initially.
- **Disk/CPU/Memory:** One `anti_pattern_check` query per tool call, adding approximately 1-2ms latency.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| In-session anti-pattern checking | P1 | M | Prevents agents from repeating known mistakes within a session |

## Trade-offs

**Why not block on first match?** Anti-patterns use a 50% word-match threshold, which is coarse and may produce false positives. Warning first, then blocking on repeat, gives the agent a chance to proceed if the match is spurious. This balances safety against over-restriction.

**Why not use a separate data store for hit counts?** Working memory is already session-scoped and expires naturally. Adding a separate store would increase complexity without benefit, since hit counts are only meaningful within a single session.

**Why 3 hits before blocking?** This is a pragmatic threshold. One hit could be a false positive. Two hits suggest a pattern. Three hits strongly indicate the agent is repeating a known mistake and should be stopped.
