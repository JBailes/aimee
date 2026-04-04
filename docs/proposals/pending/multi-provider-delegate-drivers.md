# Proposal: Multi-Provider Delegate Drivers

## Problem

Aimee's delegate system currently supports two backends: Claude Code (codex) and Ollama. Adding a new LLM provider requires building a complete integration from scratch — there's no shared abstraction for provider capabilities like streaming, tool calling, or context limits.

ayder-cli demonstrates the value of a multi-provider architecture with 7 native drivers (Ollama, OpenAI, Anthropic, Gemini, DeepSeek, Qwen/DashScope, GLM/ZhipuAI), each implementing a common provider protocol. Key design points:
- A `ProviderProtocol` base defines the contract: `stream_chat()`, `get_models()`, context window, tool-calling support
- An orchestrator factory creates the right driver from a config profile name
- Each profile stores driver type, base URL, API key, model, and context size
- The `/provider` command hot-swaps between configured profiles at runtime
- Models that lack native function calling fall back to XML tool-call parsing

This maps directly to aimee's delegate routing. Today's routing picks the cheapest enabled delegate for a role. With native provider drivers, aimee could:
1. Route delegates to any LLM provider, not just codex and ollama
2. Use OpenAI-compatible endpoints (Together, DeepInfra, Groq) for cost-effective delegation
3. Support Gemini's 1M context for large-context tasks
4. Use local models via Ollama for privacy-sensitive or offline work
5. Complement the tiered-cost-routing proposal with actual provider diversity

Evidence:
- `agent_coord.c` delegates via `codex` or `ollama` backends only
- `agent_config.c` hardcodes provider-specific setup for each backend
- Adding a new provider (e.g., Gemini, OpenAI) would require duplicating significant integration code
- The tiered-cost-routing proposal (pending) assumes multiple cost tiers exist but doesn't address adding new providers

## Goals

- A common delegate driver interface abstracts provider differences (streaming, tool calling, context limits).
- New LLM providers can be added by implementing the driver interface — no changes to delegate routing or session management.
- Profile-based configuration allows multiple provider setups (e.g., local Ollama, cloud OpenAI, cloud Gemini) to coexist.
- Delegate routing can target specific providers by profile name, or let the router pick based on role/tier/cost.
- At minimum, add OpenAI-compatible driver (covers OpenAI, DeepSeek, Together, Groq, DeepInfra) and Gemini driver alongside existing codex and ollama.

## Approach

### 1. Driver interface

Define a common interface in `src/headers/delegate_driver.h`:

```c
typedef struct delegate_driver {
    const char *name;            // "codex", "ollama", "openai", "gemini"
    
    // Initialize from config profile
    int (*init)(struct delegate_driver *self, const config_profile_t *profile);
    
    // Send a message and stream response. Calls on_chunk for each token.
    int (*stream_chat)(struct delegate_driver *self,
                       const message_t *messages, int n_messages,
                       const tool_def_t *tools, int n_tools,
                       stream_callback_t on_chunk, void *userdata);
    
    // Parse tool calls from response (native JSON or XML fallback)
    int (*parse_tool_calls)(struct delegate_driver *self,
                            const char *response,
                            tool_call_t *out, int max_calls);
    
    // Query available models
    int (*list_models)(struct delegate_driver *self,
                       model_info_t *out, int max_models);
    
    // Cleanup
    void (*destroy)(struct delegate_driver *self);
    
    // Capabilities
    int max_context_tokens;
    int supports_native_tools;   // 1 = JSON function calling, 0 = XML fallback
    int supports_streaming;
} delegate_driver_t;
```

### 2. Config profiles

Extend aimee config to support named provider profiles:

```
[delegate.profiles.local_ollama]
driver = "ollama"
base_url = "http://localhost:11434"
model = "qwen3-coder:latest"
max_context = 65536

[delegate.profiles.openai_cloud]
driver = "openai"
api_key_env = "OPENAI_API_KEY"
model = "gpt-4o"
max_context = 128000

[delegate.profiles.gemini]
driver = "gemini"
api_key_env = "GEMINI_API_KEY"
model = "gemini-3-pro"
max_context = 1000000

[delegate.profiles.deepseek]
driver = "openai"  # OpenAI-compatible
base_url = "https://api.deepseek.com/v1"
api_key_env = "DEEPSEEK_API_KEY"
model = "deepseek-coder"
max_context = 128000
```

### 3. Driver registry

```c
// Register built-in drivers at startup
void delegate_drivers_init(void);

// Look up driver by name
delegate_driver_t *delegate_driver_create(const char *driver_name);
```

### 4. OpenAI-compatible driver

Covers OpenAI, DeepSeek, Together, Groq, and any provider with an OpenAI-compatible chat completions API. Uses `agent_http.c` for HTTP, parses SSE streaming responses, and supports native tool calling via the standard `tools` parameter.

### 5. XML tool-call fallback

For models that don't support native function calling (some Ollama models, older APIs), implement XML parsing as a fallback:

```
<function=tool_name><parameter=key>value</parameter></function>
```

This matches ayder-cli's `chat_protocol = "xml"` fallback and ensures even basic models can use aimee's tool system.

### Changes

| File | Change |
|------|--------|
| `src/headers/delegate_driver.h` (new) | Driver interface definition |
| `src/delegate_driver.c` (new) | Driver registry, factory, shared utilities |
| `src/delegate_openai.c` (new) | OpenAI-compatible driver (covers OpenAI, DeepSeek, Together, Groq) |
| `src/delegate_gemini.c` (new) | Gemini driver via Google GenAI REST API |
| `src/delegate_xml_fallback.c` (new) | XML tool-call parser for non-native-tool models |
| `src/agent_coord.c` | Use driver interface instead of hardcoded codex/ollama dispatch |
| `src/agent_config.c` | Load profiles from config, create drivers from profiles |
| `src/config.c` | Parse `[delegate.profiles.*]` sections |

## Acceptance Criteria

- [ ] `delegate_driver_t` interface is implemented by at least 4 drivers: codex, ollama, openai, gemini
- [ ] Delegates can be routed to any configured profile via `aimee delegate --profile <name> <role> "task"`
- [ ] OpenAI-compatible driver works with OpenAI, DeepSeek, and at least one other compatible API
- [ ] XML tool-call fallback works for models without native function calling
- [ ] Profile configuration is validated at startup (missing API keys, unreachable URLs produce clear errors)
- [ ] Existing codex and ollama delegation behavior is unchanged (backward compatible)

## Owner and Effort

- **Owner:** delegate (code)
- **Effort:** L (5-7 days)
- **Dependencies:** None directly, but benefits from tiered-cost-routing proposal for intelligent profile selection

## Rollout and Rollback

- **Rollout:** Additive — existing codex/ollama configs continue to work. New profiles are opt-in via config.
- **Rollback:** Remove profile config sections. Existing codex/ollama code paths are preserved as drivers, so reverting the abstraction layer is the only risk.
- **Blast radius:** A broken driver could fail delegate calls to that provider. Mitigation: driver init validates connectivity, and delegate routing falls back to next available profile on failure.

## Test Plan

- [ ] Unit tests: driver interface compliance for each driver, config parsing, XML fallback parsing
- [ ] Integration tests: delegate task via OpenAI-compatible endpoint, verify tool call round-trip
- [ ] Failure injection: unreachable provider URL (timeout + fallback), invalid API key (clear error), model not found
- [ ] Manual verification: configure a DeepSeek profile, delegate a code task, verify completion

## Operational Impact

- **Metrics:** `delegate_calls_by_driver{driver="openai"}`, `delegate_driver_errors_total`
- **Logging:** Driver init at INFO, API calls at DEBUG, failures at WARN
- **Alerts:** None initially
- **Disk/CPU/Memory:** Minimal — drivers are lightweight wrappers around HTTP calls. No persistent state beyond config.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Driver interface + registry | P2 | M | High — enables everything else |
| OpenAI-compatible driver | P2 | M | High — covers most cloud providers |
| Gemini driver | P3 | M | Medium — unique for large-context tasks |
| XML fallback | P3 | S | Medium — enables local model tools |
| Profile config | P2 | S | High — user-facing configuration |

## Trade-offs

- **Why not just use MCP for all providers?** MCP adds protocol overhead and requires persistent server processes. Native drivers are simpler, faster, and give aimee direct control over streaming, tool calling, and error handling. MCP is better for extensibility; native drivers are better for core provider support.
- **Why an OpenAI-compatible driver instead of per-provider drivers?** The OpenAI chat completions API is a de facto standard. DeepSeek, Together, Groq, Mistral, and others all implement it. One driver covers many providers.
- **Why not wait for tiered-cost-routing?** The driver interface is a prerequisite for meaningful cost routing — you can't route to cheaper providers if you can't talk to them. This proposal provides the infrastructure that cost routing builds on.

## Source Reference

Implementation reference: ayder-cli `src/ayder_cli/providers/` — `base.py` (AIProvider protocol), `orchestrator.py` (factory), and `impl/` directory with 7 native drivers.
