# Proposal: Delegation Attempt Log

## Problem

When an agent tries an approach that fails (wrong API, build error, test failure), that knowledge is trapped in the agent's conversation context. If the agent delegates a related task, the delegate has no visibility into what was already tried and failed. This leads to repeated failures, wasted compute, and frustrated users watching the same mistake twice.

Working memory (src/working_memory.c) provides session-scoped key-value storage, but there is no convention or tooling for recording failed attempts in a structured, queryable way. Agents would need to manually format and store attempt data, which they rarely do.

## Goals

- Failed approaches are automatically recorded in a structured format.
- Delegates can query 'what has already been tried for this task?' before starting work.
- Attempt records include: what was tried, why it failed, and what to avoid.

## Approach

### 1. Attempt record structure

Define a structured attempt record stored in working memory:

| Field | Type | Description |
|-------|------|-------------|
| task_context | string | Brief description of what was being attempted |
| approach | string | What was tried |
| outcome | string | What happened (error message, test failure, etc.) |
| lesson | string | What to avoid or do differently |
| timestamp | string | When this was recorded |

### 2. Recording API

Add a server method attempt.record that stores an attempt in working memory with category 'attempt' and a TTL matching the session lifetime:

On the server side, this maps to a wm_set() call with structured JSON content.

### 3. Query API

Add attempt.list that returns all attempts for the current session, optionally filtered by task_context keyword. Delegates receive attempt history as part of their assembled context when the delegation prompt matches attempt task_context keywords.

### 4. Context integration

In agent_build_exec_context(), when assembling delegate context, query working memory for category='attempt' entries. Include matching attempts in a 'Previous Attempts' section of the context, budget-permitting.

### Changes

| File | Change |
|------|--------|
| src/server.c | Register attempt.record and attempt.list dispatch methods |
| src/server_compute.c | Implement attempt_record() and attempt_list() handlers |
| src/agent_context.c | Query attempt entries and include in delegate context |
| src/mcp_server.c | Expose record_attempt and list_attempts as MCP tools |

## Acceptance Criteria

- [ ] attempt.record stores structured attempt data in working memory
- [ ] attempt.list returns attempts for the current session
- [ ] Delegates receive relevant attempt history in their context
- [ ] Attempt records expire with the session (no long-term pollution)
- [ ] MCP tools are available for external agents to record/query attempts

## Owner and Effort

- **Owner:** TBD
- **Effort:** S-M
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Direct code change. New server methods and MCP tools.
- **Rollback:** git revert. Attempt data in working memory is session-scoped and self-cleaning.
- **Blast radius:** None. Additive feature, no changes to existing behavior.

## Test Plan

- [ ] Unit test: attempt.record stores and attempt.list retrieves correctly
- [ ] Unit test: keyword filtering in attempt.list works
- [ ] Integration test: delegate context includes relevant attempts
- [ ] Integration test: attempts expire with session
- [ ] Manual verification: record an attempt, delegate a task, verify delegate sees it

## Operational Impact

- **Metrics:** None.
- **Logging:** None.
- **Alerts:** None.
- **Disk/CPU/Memory:** Attempts stored in working_memory table with TTL. Negligible overhead.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Attempt log | P2 | S-M | Prevents repeated delegate failures |

## Trade-offs

Using working memory (session-scoped) rather than long-term memory is intentional. Attempt records are highly contextual and lose value across sessions. A failed approach in one session may be valid in another after code changes. Session scope prevents stale 'avoid this' advice from poisoning future work.
