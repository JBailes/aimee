# Proposal: Tool-Pair Validator (Orphaned Tool Calls)

## Problem

When context compaction occurs mid-session, tool_use blocks can become orphaned — their corresponding tool_result was in a compacted region and is gone. The LLM API requires every tool_use to have a matching tool_result. Orphaned tool_use blocks cause API errors that crash the delegate session, losing all progress.

Evidence: oh-my-openagent implements a tool-pair validator (`src/hooks/tool-pair-validator/hook.ts`) that scans the message history for orphaned tool_use blocks and injects placeholder tool_results ("Tool output unavailable (context compacted)") before sending to the API.

## Goals

- Detect orphaned tool_use blocks (no matching tool_result in message history)
- Inject stub tool_results to maintain API contract
- Prevent API errors from crashing delegate sessions after compaction
- Transparent to the agent — it sees the stub and can re-invoke if needed

## Approach

Add a message-transform pass before API calls that scans for unpaired tool_use/tool_result blocks. For any tool_use without a matching tool_result, inject a placeholder result.

### Changes

| File | Change |
|------|--------|
| `src/agent_context.c` | Add `context_validate_tool_pairs()` before API dispatch |
| `src/agent_eval.c` | Call validation after compaction events |

## Acceptance Criteria

- [ ] Orphaned tool_use blocks get stub tool_results injected
- [ ] Paired tool_use/tool_result blocks are not affected
- [ ] Stub content clearly indicates the result was compacted
- [ ] API calls succeed after compaction without tool-pair errors
- [ ] Validation adds <10ms even for sessions with hundreds of tool calls

## Owner and Effort

- **Owner:** aimee
- **Effort:** M (1–2 days)
- **Dependencies:** Context compaction must be implemented

## Rollout and Rollback

- **Rollout:** Compile-time inclusion; runs automatically before API calls
- **Rollback:** Remove validation; API errors on orphaned pairs as before
- **Blast radius:** Only affects message serialization; no behavioral change for complete pairs

## Test Plan

- [ ] Unit test: orphaned tool_use gets stub result
- [ ] Unit test: complete pair is unchanged
- [ ] Unit test: multiple orphans in one session all get stubs
- [ ] Unit test: stub content is parseable by the agent
- [ ] Integration test: simulate compaction, verify session continues

## Operational Impact

- **Metrics:** Count of stub injections per session
- **Logging:** Log stub injection at debug level
- **Disk/CPU/Memory:** One scan of message history per API call; linear in message count

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Tool-Pair Validator | P2 | M | Medium — prevents session crashes after compaction |

## Trade-offs

Alternative: prevent compaction from removing tool_results (mark them as pinned). More robust but constrains the compaction algorithm and may prevent effective context reduction. Stub injection is non-invasive.

Inspiration: oh-my-openagent `src/hooks/tool-pair-validator/hook.ts`
