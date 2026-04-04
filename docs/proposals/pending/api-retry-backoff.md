# Proposal: Exponential Backoff with Overflow-Safe API Retries

## Problem

Aimee's `agent_http.c` makes API calls to LLM providers but lacks structured retry logic. When a provider returns 429 (rate limit), 502 (bad gateway), or 503 (service unavailable), the request fails immediately. Users see transient errors that would resolve with a retry.

This affects all execution surfaces equally — CLI delegates, webchat sessions, and MCP tool calls all go through the same HTTP path.

The `soongenwong/claudecode` repo at `rust/crates/api/src/providers/openai_compat.rs` implements overflow-safe exponential backoff with a curated retryable status code set.

## Goals

- Transient API errors (429, 408, 409, 500, 502, 503, 504) are automatically retried with exponential backoff.
- Backoff uses overflow-safe arithmetic (no integer overflow on high retry counts).
- Max retries and max backoff ceiling are configurable.
- Retry behavior is transparent — logged at INFO so users can see what's happening in both CLI and webchat.

## Approach

### Retryable Status Codes

```c
static const int RETRYABLE_STATUS[] = {
    408, /* Request Timeout */
    409, /* Conflict (temporary contention) */
    429, /* Too Many Requests (rate limit) */
    500, /* Internal Server Error */
    502, /* Bad Gateway */
    503, /* Service Unavailable */
    504, /* Gateway Timeout */
    0
};
```

### Backoff Algorithm

```c
int backoff_ms(int attempt, int base_ms, int max_ms) {
    int delay = base_ms;
    for (int i = 0; i < attempt && delay < max_ms; i++) {
        if (delay > max_ms / 2) { delay = max_ms; break; }
        delay *= 2;
    }
    return delay;
}
```

Default: base=1000ms, max=30000ms, max_retries=3.

### Changes

| File | Change |
|------|--------|
| `src/http_retry.c` (new) | Retryable status classification, overflow-safe backoff, retry loop wrapper |
| `src/headers/http_retry.h` (new) | Public API: `http_should_retry()`, `http_backoff_ms()`, `http_retry_request()` |
| `src/agent_http.c` | Wrap API calls with `http_retry_request()` |
| `src/server_compute.c` | Apply retry logic to provider API calls |
| `src/config.c` | Add `retry_max_attempts`, `retry_base_ms`, `retry_max_ms` config |

## Acceptance Criteria

- [ ] 429 responses are retried up to 3 times with exponential backoff
- [ ] 502/503/504 responses are retried
- [ ] Non-retryable errors (400, 401, 403, 404) fail immediately
- [ ] Backoff does not overflow for any retry count
- [ ] Retry attempts are logged at INFO (visible in CLI and webchat)
- [ ] Works for CLI delegates, webchat sessions, and MCP tool calls

## Owner and Effort

- **Owner:** delegate (code)
- **Effort:** S (1 day)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Default enabled with 3 retries. Set `retry_max_attempts: 0` to disable.
- **Rollback:** Set max_attempts to 0.
- **Blast radius:** LLM API calls are idempotent so retries are safe.

## Test Plan

- [ ] Unit tests: backoff calculation, overflow safety, status code classification
- [ ] Integration tests: mock server returning 429 then 200
- [ ] Manual verification: trigger rate limit, observe retry in logs

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Retry logic + backoff | P2 | S | High — eliminates transient failures for all surfaces |

## Source Reference

Implementation reference: `soongenwong/claudecode` at `rust/crates/api/src/providers/openai_compat.rs`.
