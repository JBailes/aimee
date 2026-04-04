# Proposal: Per-Model Concurrency Limits

## Problem

When multiple delegates target the same LLM model simultaneously, they can trigger rate limits, causing cascading failures. Rate-limited API calls return errors, delegates retry, and the retry storm makes the rate limiting worse. This is the primary cause of delegate failures during parallel execution.

Evidence: oh-my-openagent implements a `ConcurrencyManager` (`src/features/background-agent/concurrency.ts`) that enforces per-model and per-provider concurrency limits with a semaphore queue. Excess requests wait until a slot opens rather than hammering the API.

## Goals

- Limit concurrent API calls per model (default: 5)
- Limit concurrent API calls per provider (configurable)
- Queue excess requests with FIFO ordering
- Configurable per-model and per-provider limits

## Approach

Add a concurrency manager to the compute layer. Before dispatching an API call, acquire a slot for the target model. If no slots are available, queue the request. When a call completes, release the slot and wake the next queued request.

### Configuration

```yaml
# .aimee/project.yaml
compute:
  concurrency:
    default: 5
    per_model:
      ollama/qwen3: 2
    per_provider:
      anthropic: 3
```

### Changes

| File | Change |
|------|--------|
| `src/server_compute.c` | Add concurrency manager with acquire/release/queue |
| `src/config.c` | Parse concurrency limits from project.yaml |
| `src/headers/config.h` | Add concurrency config struct |

## Acceptance Criteria

- [ ] 6th concurrent request to a model with limit 5 is queued, not rejected
- [ ] Queued request proceeds when a slot is released
- [ ] Per-model limits override per-provider limits override default
- [ ] Queue is FIFO
- [ ] Session cancellation releases the slot and dequeues waiting requests

## Owner and Effort

- **Owner:** aimee
- **Effort:** M (2–3 days)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Config-driven; default limit of 5 per model
- **Rollback:** Remove concurrency manager; all requests dispatch immediately as before
- **Blast radius:** Affects API call timing only; no behavioral change to delegates

## Test Plan

- [ ] Unit test: requests within limit proceed immediately
- [ ] Unit test: requests exceeding limit are queued
- [ ] Unit test: slot release wakes queued request
- [ ] Unit test: per-model limit overrides default
- [ ] Unit test: cancelled session releases slot
- [ ] Integration test: 10 concurrent delegates, 5 model limit, verify queueing

## Operational Impact

- **Metrics:** Queue depth per model, wait time per queued request
- **Logging:** Log queue events at debug, high queue depth at warning
- **Disk/CPU/Memory:** One semaphore per model; negligible

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Per-Model Concurrency Limits | P2 | M | High — prevents rate-limit cascading failures |

## Trade-offs

Alternative: rely on model fallback chains (proposal #6) to handle rate limits. Reactive rather than preventive — it's better to avoid rate limits entirely by controlling concurrency.

Inspiration: oh-my-openagent `src/features/background-agent/concurrency.ts`
