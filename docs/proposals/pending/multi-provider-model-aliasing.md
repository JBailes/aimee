# Proposal: Multi-Provider API Client with Model Aliasing

## Problem

Aimee's compute routing (`server_compute.c`, `compute_pool.c`) supports multiple backends, but switching between providers requires explicit configuration. There is no:

1. Short alias support — users must type full model IDs like `claude-opus-4-6` instead of just `opus`.
2. Auto-detection of provider from model name — `gpt-4o` should route to OpenAI, `grok-3` to xAI, `claude-*` to Anthropic without manual config.
3. OpenAI-compatible protocol adapter — many providers (Ollama, LM Studio, vLLM, Together, Groq) expose the OpenAI chat completions API.

This affects CLI delegates, webchat sessions, and any MCP tool call that routes through compute.

The `soongenwong/claudecode` repo at `rust/crates/api/src/providers/` implements a model alias registry, provider auto-detection, and an OpenAI-compatible client adapter.

## Goals

- Users can specify models by short alias: `aimee delegate code --model opus "fix the bug"`
- Provider is auto-detected from the model name — no need to configure base URLs for known providers.
- OpenAI-compatible providers work out of the box when `OPENAI_BASE_URL` + `OPENAI_API_KEY` are set.
- Works identically from CLI and webchat.

## Approach

### Model Alias Registry

```c
static const struct model_alias {
    const char *alias;
    const char *full_id;
    const char *provider;
} MODEL_ALIASES[] = {
    {"opus",    "claude-opus-4-6",           "anthropic"},
    {"sonnet",  "claude-sonnet-4-6",         "anthropic"},
    {"haiku",   "claude-haiku-4-5-20251001", "anthropic"},
    {"gpt4o",   "gpt-4o",                   "openai"},
    {"grok",    "grok-3",                   "xai"},
    {NULL, NULL, NULL}
};
```

### Provider Auto-Detection

```
model starts with "claude-"     → Anthropic
model starts with "gpt-"        → OpenAI
model starts with "grok"        → xAI
OPENAI_BASE_URL is set          → OpenAI-compat (catch-all for local models)
```

### Changes

| File | Change |
|------|--------|
| `src/model_registry.c` (new) | Alias resolution, provider auto-detection, model capability metadata (max tokens) |
| `src/headers/model_registry.h` (new) | Public API: `resolve_model_alias()`, `detect_provider()`, `model_max_tokens()` |
| `src/openai_compat.c` (new) | OpenAI chat completions adapter: request translation, response parsing, SSE stream adaptation |
| `src/server_compute.c` | Use `resolve_model_alias()` + `detect_provider()` for routing |
| `src/config.c` | Add per-provider env-var config, custom alias overrides |

## Acceptance Criteria

- [ ] `aimee delegate code --model opus "task"` resolves to `claude-opus-4-6` on Anthropic
- [ ] `aimee delegate code --model gpt4o "task"` resolves to `gpt-4o` on OpenAI
- [ ] Setting `OPENAI_BASE_URL=http://localhost:11434/v1` routes to local Ollama
- [ ] Unknown model names pass through unchanged
- [ ] Works from CLI delegates and webchat sessions alike

## Owner and Effort

- **Owner:** delegate (code)
- **Effort:** M (2-3 days)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Additive — all existing model names and config continue to work.
- **Rollback:** Remove alias resolution — full model IDs still work.
- **Blast radius:** If alias resolution is wrong, the wrong model gets called. Mitigation: log resolved model name at INFO.

## Test Plan

- [ ] Unit tests: alias resolution, provider auto-detection, unknown model passthrough
- [ ] Integration tests: OpenAI-compat adapter with mock server
- [ ] Manual verification: `aimee delegate code --model opus "echo hello"` produces output from Claude

## Operational Impact

- **Metrics:** `model_alias_resolutions_total`, `provider_detections` by provider name
- **Logging:** Alias resolution at DEBUG
- **Alerts:** None
- **Disk/CPU/Memory:** Negligible — static lookup table

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Model alias registry | P2 | S | High — UX improvement |
| Provider auto-detection | P2 | S | High — eliminates manual config |
| OpenAI-compat adapter | P2 | M | High — unlocks Ollama/vLLM/etc |

## Trade-offs

- **Why not LiteLLM?** Python dependency. A C lookup table is zero-dependency and instant.
- **Why include model capability metadata?** `max_tokens` per model is needed for session compaction to know when to trigger.

## Source Reference

Implementation reference: `soongenwong/claudecode` at `rust/crates/api/src/providers/`.
