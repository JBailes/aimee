# Proposal: Model Fallback Chains

## Problem

When the primary model for a delegate session fails (rate limit, overload, outage), the session fails entirely. The user must manually retry or reconfigure. This is especially painful for long-running delegates that fail partway through — all progress is lost.

Evidence: oh-my-openagent implements model fallback chains (`src/hooks/model-fallback/hook.ts`). Each agent role has a chain of provider/model pairs. On failure, the system automatically switches to the next model in the chain and retries.

## Goals

- Automatic model failover when the primary model returns a retryable error
- Configurable fallback chains per compute backend
- Preserve session state across model switches (no restart)
- Support at least 3 models in a chain

## Approach

Add a fallback chain to the compute layer. When a model API call fails with a retryable error (429, 503, 529, timeout), consult the fallback chain for the next model. Retry the same request with the fallback model. If all models in the chain fail, report the error as before.

### Fallback chain configuration

```yaml
# .aimee/project.yaml
compute:
  fallback_chains:
    default: [codex, claude-sonnet, ollama/qwen3]
    draft: [ollama/qwen3, codex]
```

### Changes

| File | Change |
|------|--------|
| `src/server_compute.c` | Add fallback chain lookup on retryable errors; retry with next model |
| `src/config.c` | Parse fallback chain config from project.yaml |
| `src/headers/config.h` | Add fallback chain config struct |

## Acceptance Criteria

- [ ] Retryable API errors (429, 503, timeout) trigger fallback to next model
- [ ] Non-retryable errors (400, 401) do not trigger fallback
- [ ] Fallback chain is configurable per role
- [ ] Session context is preserved across model switch
- [ ] All models in chain failing produces a clear error

## Owner and Effort

- **Owner:** aimee
- **Effort:** M (2–3 days)
- **Dependencies:** Multiple compute backends must be configured

## Rollout and Rollback

- **Rollout:** Config-driven; no fallback chain = current behavior (single model, fail on error)
- **Rollback:** Remove fallback chain config; revert to single-model behavior
- **Blast radius:** Affects compute layer error handling only

## Test Plan

- [ ] Unit test: 429 triggers fallback to next model
- [ ] Unit test: 400 does not trigger fallback
- [ ] Unit test: all models failing returns error
- [ ] Unit test: chain with one model behaves like current behavior
- [ ] Integration test: simulate rate limit, verify automatic failover

## Operational Impact

- **Metrics:** Fallback trigger count per model, per session
- **Logging:** Log model switch at warning level
- **Disk/CPU/Memory:** One chain lookup per failed API call; negligible

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Model Fallback Chains | P2 | M | Medium — improves delegate reliability during outages |

## Trade-offs

Alternative: retry the same model with exponential backoff. Works for transient errors but not for sustained outages or rate limits. Fallback chains provide immediate recovery by switching to a different provider.

Alternative: let the user configure fallbacks at the CLI level. Misses the automation — the whole point is that long-running delegates recover without human intervention.

Inspiration: oh-my-openagent `src/hooks/model-fallback/hook.ts`
