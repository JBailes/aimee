# Proposal: Consecutive Message Compaction

## Problem

When aimee injects system reminders, guardrail warnings, or middleware messages into the conversation, it can create consecutive messages with the same role (e.g., two `user` messages in a row, or two `system` messages). Some LLM APIs handle this poorly — OpenAI's API rejects consecutive same-role messages in certain configurations, and Anthropic's API merges them silently but may lose structure.

Mistral-vibe implements `merge_consecutive_user_messages()` which scans the message array before each LLM call and concatenates adjacent same-role messages into a single message, preserving the first message's ID.

## Goals

- No consecutive same-role messages are sent to the LLM API.
- Adjacent same-role messages are merged with a double-newline separator.
- Merging preserves the first message's metadata (ID, timestamp).
- Works for all roles (user, assistant, system).
- Applied consistently in CLI chat and webchat before every LLM call.

## Approach

### Merge Function

Before each LLM call, scan the messages array. When two adjacent messages share the same role, concatenate their content with `\n\n` and remove the duplicate. Repeat until no consecutive same-role pairs remain.

```c
/* Merge consecutive same-role messages in-place.
 * Returns the new message count. */
int messages_compact_consecutive(cJSON *messages);
```

### Integration

Call `messages_compact_consecutive()` in the same location as the proposed message history repair — right before building the LLM request in both `cmd_chat.c` and `webchat.c`.

### Changes

| File | Change |
|------|--------|
| `src/agent.c` | Add `messages_compact_consecutive()` |
| `src/cmd_chat.c` | Call before LLM request |
| `src/webchat.c` | Call before LLM request |

## Acceptance Criteria

- [ ] No consecutive same-role messages are sent to any LLM API
- [ ] Merged messages preserve the first message's ID/metadata
- [ ] Content is joined with double newline
- [ ] Function is idempotent
- [ ] Works for user, assistant, and system roles
- [ ] Applied in both CLI and webchat paths

## Owner and Effort

- **Owner:** aimee
- **Effort:** XS (half day)
- **Dependencies:** None (pairs well with message-history-repair proposal)

## Rollout and Rollback

- **Rollout:** Automatic. No config needed.
- **Rollback:** Remove calls. May cause API errors with some providers.
- **Blast radius:** All LLM calls. Low risk — strictly improves API compatibility.

## Test Plan

- [ ] Unit tests: 0, 1, 2, 5 consecutive same-role messages
- [ ] Unit tests: mixed roles (user-user-assistant-user-user)
- [ ] Unit tests: idempotency
- [ ] Integration tests: inject guardrail warnings that create consecutive user messages, verify merged

## Operational Impact

- **Metrics:** `messages_merged` counter (DEBUG)
- **Logging:** DEBUG when merges occur
- **Alerts:** None
- **Disk/CPU/Memory:** Negligible — single-pass array scan

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Consecutive Message Compaction | P2 | XS | Medium — prevents subtle API errors and confused LLM responses |

## Trade-offs

**Alternative: Prevent consecutive same-role messages at injection time.** Harder to enforce across all injection points (guardrails, middleware, tool results). Post-hoc compaction is simpler and catches all cases.

**Known limitation:** Merging may lose semantic boundaries between originally separate messages. The double-newline separator mitigates this, and in practice the merged content is functionally equivalent.
