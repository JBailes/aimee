# Proposal: Message History Repair

## Problem

During long agent sessions, message history can become inconsistent:

1. **Orphaned tool calls**: An assistant message references tool call IDs, but the corresponding tool results are missing (connection drop, crash, timeout during tool execution).
2. **Orphaned tool results**: Tool result messages exist without a matching assistant tool call (state corruption, partial resume).
3. **Missing stop conditions**: The conversation ends mid-tool-call with no assistant follow-up, confusing the LLM on resume.

When these inconsistencies are sent to the LLM, it produces confused responses, hallucinates tool results, or errors out. This is especially problematic for session resume (where old conversations are reloaded) and long-running autonomous sessions.

Mistral-vibe implements `_clean_message_history()` and `_fill_missing_tool_responses()` which run before each LLM call to ensure every tool call has a matching tool result (filling missing ones with cancellation messages) and removes orphaned entries.

## Goals

- Message history is always consistent before sending to the LLM.
- Orphaned tool calls get synthetic "cancelled" results.
- Orphaned tool results are removed.
- Repair is automatic and silent — no user intervention needed.
- Works identically in CLI chat and webchat.

## Approach

### Repair Functions

Run before each LLM call (both in `cmd_chat.c` and `webchat.c`):

1. **Scan for orphaned tool calls**: For each assistant message containing `tool_use` blocks, check that a corresponding `tool_result` message exists. If missing, insert a synthetic result:
   ```json
   {"type": "tool_result", "tool_use_id": "<id>", "content": "[Tool call was cancelled or timed out]"}
   ```

2. **Remove orphaned tool results**: For each `tool_result` message, verify a matching `tool_use` exists in a preceding assistant message. If not, remove the orphan.

3. **Trailing tool call check**: If the conversation ends with an assistant message containing unanswered tool calls, fill them all with cancellation results before the next LLM call.

### When to Run

- Before every LLM API call (lightweight — linear scan of messages).
- On session resume (before presenting loaded messages to the LLM).
- After connection recovery (if the SSE stream dropped mid-tool-execution).

### Changes

| File | Change |
|------|--------|
| `src/headers/agent.h` | Add `message_history_repair()` API |
| `src/agent.c` | Implement repair logic, call before LLM requests |
| `src/cmd_chat.c` | Call repair before chat LLM calls |
| `src/webchat.c` | Call repair before webchat LLM calls |

## Acceptance Criteria

- [ ] Orphaned tool calls get synthetic cancellation results
- [ ] Orphaned tool results are removed from history
- [ ] Trailing unanswered tool calls are filled before next LLM call
- [ ] Repair runs automatically — no user command needed
- [ ] Repair is idempotent (running twice produces same result)
- [ ] Repair works correctly for both OpenAI and Anthropic message formats
- [ ] CLI and webchat both use the same repair function
- [ ] Repair logged at DEBUG level (silent to user)

## Owner and Effort

- **Owner:** aimee
- **Effort:** S (1-2 days)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Automatic. No config needed.
- **Rollback:** Remove repair calls. History may have inconsistencies but won't crash.
- **Blast radius:** All LLM calls. Must handle both OpenAI and Anthropic message formats correctly.

## Test Plan

- [ ] Unit tests: repair with orphaned tool calls, orphaned results, trailing calls, mixed
- [ ] Unit tests: verify idempotency
- [ ] Integration tests: simulate crash mid-tool-execution, resume session, verify repair
- [ ] Integration tests: both OpenAI and Anthropic message format variants
- [ ] Failure injection: corrupt message history, verify graceful repair
- [ ] Manual verification: kill chat mid-tool-call, restart, observe clean continuation

## Operational Impact

- **Metrics:** `message_history_repairs{type=orphan_call|orphan_result|trailing_call}` counter
- **Logging:** DEBUG per repair action, INFO summary (e.g., "repaired 2 orphaned tool calls")
- **Alerts:** WARN if repair count exceeds threshold (suggests systemic issue)
- **Disk/CPU/Memory:** Negligible — linear scan of message array, typically <100 messages.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Message History Repair | P1 | S | High — prevents confused LLM responses and enables reliable session resume |

## Trade-offs

**Alternative: Prevent inconsistencies instead of repairing them.** Ideal but impossible to guarantee — network drops, process kills, and OOM kills can interrupt at any point. Repair is defense-in-depth.

**Alternative: Discard and restart the session on inconsistency.** Too aggressive — users lose context. Repair preserves as much context as possible.

**Known limitation:** Synthetic cancellation results may confuse the LLM about why a tool was cancelled. The cancellation message is explicit ("cancelled or timed out") to minimize confusion.
