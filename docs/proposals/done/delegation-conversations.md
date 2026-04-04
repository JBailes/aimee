# Proposal: Delegation Conversations

## Problem

Delegation in aimee is fire-and-forget: the parent agent sends a prompt, the delegate executes, and a final result is returned (server_compute.c:delegate_worker). There is no mechanism for the parent to:

1. Course-correct mid-task ('actually, use the v2 API, not v1')
2. Answer delegate questions ('which database should I target?')
3. Provide incremental feedback ('the first file looks good, but the second needs X')

This means delegates must be fully self-sufficient from a single prompt. Complex tasks that require judgment calls or clarification either fail or produce wrong results that must be redone from scratch.

## Goals

- Parent agents can send follow-up messages to an in-progress delegation.
- Delegates can signal 'I need input' and pause for parent response.
- The delegation protocol supports multi-turn conversations without breaking the existing fire-and-forget path.

## Approach

### 1. Delegation mailbox

Add a per-delegation message queue in the database:

CREATE TABLE IF NOT EXISTS delegation_messages (
  id INTEGER PRIMARY KEY,
  delegation_id TEXT NOT NULL,
  direction TEXT NOT NULL,
  content TEXT NOT NULL,
  created_at TEXT NOT NULL DEFAULT (datetime('now'))
);

direction is 'parent_to_delegate' or 'delegate_to_parent'.

### 2. Delegate pause/resume

When a delegate needs input, it calls a new tool request_input with a question. This:
1. Inserts a delegate_to_parent message
2. Sets the delegation status to 'waiting_input'
3. Blocks the delegate thread (condition variable wait with timeout)

The parent polls or is notified of the waiting status.

### 3. Parent reply

New server method delegate.reply:
1. Inserts a parent_to_delegate message
2. Signals the condition variable to wake the delegate
3. The delegate receives the reply as the return value of request_input

### 4. Timeout and fallback

If no reply arrives within a configurable timeout (default: 60s), the delegate receives a timeout signal and must proceed with its best judgment. This prevents indefinite blocking.

### 5. Backward compatibility

Existing delegations that never call request_input work identically to today. The mailbox and pause/resume are opt-in by the delegate.

### Changes

| File | Change |
|------|--------|
| src/db.c | Add delegation_messages table migration |
| src/server_compute.c | Add pause/resume logic with condition variable in delegate_worker |
| src/server.c | Register delegate.reply dispatch method |
| src/agent_tools.c | Add tool_request_input() for delegates |
| src/headers/agent_tools.h | Declare tool_request_input |
| src/mcp_server.c | Expose delegate_reply as MCP tool for parent agents |

## Acceptance Criteria

- [ ] Delegates can call request_input to pause and receive parent replies
- [ ] Parent agents can send replies via delegate.reply
- [ ] Timeout fires after configurable period if parent does not reply
- [ ] Existing fire-and-forget delegations work unchanged
- [ ] Multiple request_input calls within one delegation work correctly
- [ ] Message history is queryable for debugging

## Owner and Effort

- **Owner:** TBD
- **Effort:** M-L
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Database migration adds table. New server methods. Feature is opt-in by delegates.
- **Rollback:** Revert commit + reverse migration. Existing delegations unaffected.
- **Blast radius:** Only delegations that use request_input. A bug in the condition variable logic could deadlock the compute thread. The timeout is the safety net.

## Test Plan

- [ ] Unit test: message insert/query roundtrip
- [ ] Unit test: condition variable signal/wait with timeout
- [ ] Integration test: delegate pauses, parent replies, delegate resumes
- [ ] Integration test: delegate pauses, timeout fires, delegate continues
- [ ] Integration test: multiple request_input calls in one delegation
- [ ] Failure injection: parent disconnects while delegate is waiting
- [ ] Manual verification: end-to-end with a real agent

## Operational Impact

- **Metrics:** None.
- **Logging:** Log pause/resume events and timeouts.
- **Alerts:** None.
- **Disk/CPU/Memory:** One condition variable per active delegation. Message table grows with delegation count but is small (text messages only).

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Delegation conversations | P2 | M-L | Enables complex multi-step delegations |

## Trade-offs

Condition variables add threading complexity. An alternative is polling (delegate checks for replies on a timer), which is simpler but adds latency and wastes CPU. The condition variable approach gives instant wakeup with minimal resource use.

The 60s default timeout is a trade-off between giving parents time to respond and not blocking compute threads indefinitely. This is configurable for different use cases.
