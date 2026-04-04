# Proposal: Built-in CLI with Multi-Provider Support

## Problem

After adding the built-in OpenAI-compatible chat CLI, users who select Claude, Gemini, or Codex as their primary agent are still required to have the native CLI tool installed. This is a barrier:

1. **Installation friction.** Claude Code, Gemini CLI, and Codex CLI each have their own installation process, dependencies, and authentication flows. Users who already have API keys may not want to install a separate CLI.

2. **No choice of interface.** Users are locked into the native CLI's UX. Some may prefer aimee's built-in chat, which provides a consistent experience across all providers with full control over tool definitions and execution.

3. **API-only users.** Developers who interact with these models via API (e.g., for cost control, using specific model versions, or running against custom endpoints) have no path through aimee's launcher.

## Goals

- After selecting claude/gemini/codex, users choose: use the native CLI or use aimee's built-in chat.
- The built-in chat routes traffic through the selected provider's API with correct format, auth, and tool definitions.
- Claude (Anthropic) uses the Anthropic Messages API with native streaming format.
- Gemini uses Google's OpenAI-compatible endpoint, avoiding a third streaming format.
- Codex uses the standard OpenAI chat/completions format.
- The installer prompts for provider-specific endpoint, model, and API key.
- Native CLI users are unaffected (backward compatible).

## Approach

### 1. Config: `use_builtin_cli` flag

A new boolean field `use_builtin_cli` in `config_t` and `config.json`. When true, `aimee` launches the built-in chat instead of exec'ing the native CLI. Default is false. The `provider` field continues to indicate which provider is selected; `use_builtin_cli` only controls the interface choice.

### 2. Provider detection in `cmd_chat`

The chat CLI detects the provider from `cfg.provider` and selects the appropriate API format:

| Provider | Format | Endpoint default | Model default | Auth |
|----------|--------|-----------------|---------------|------|
| `openai` | OpenAI chat/completions | `api.openai.com/v1` | `gpt-4o` | `Authorization: Bearer` |
| `claude` | Anthropic Messages API | `api.anthropic.com/v1` | `claude-sonnet-4-20250514` | `x-api-key` |
| `gemini` | OpenAI-compat (Google) | `generativelanguage.googleapis.com/v1beta/openai` | `gemini-2.5-pro` | `Authorization: Bearer` |
| `codex` | OpenAI chat/completions | `api.openai.com/v1` | `o3` | `Authorization: Bearer` |

Defaults are only applied when the config still has the original OpenAI defaults (`api.openai.com/v1`, `gpt-4o`), so user customizations are preserved.

### 3. Anthropic streaming format

The Anthropic Messages API uses a different SSE event structure than OpenAI:

- **Events**: `content_block_start`, `content_block_delta`, `content_block_stop`, `message_delta`, `message_stop`
- **Text streaming**: `content_block_delta` with `delta.type == "text_delta"` and `delta.text`
- **Tool calls**: `content_block_start` with `content_block.type == "tool_use"` (carries id and name), followed by `content_block_delta` with `delta.type == "input_json_delta"` (carries partial JSON arguments)
- **Stop reason**: `message_delta` with `delta.stop_reason` (`"end_turn"` or `"tool_use"`)
- **Request format**: System prompt in `system` field (not in messages array), `max_tokens` required, tools use `input_schema` instead of `parameters`

The parser dispatches between `process_sse_line_openai()` and `process_sse_line_anthropic()` based on the provider enum in the chat state.

### 4. Tool call history format

Anthropic and OpenAI use different JSON structures for tool calls in conversation history:

**OpenAI**: Assistant message has `tool_calls` array with function objects. Each tool result is a separate `tool` role message referencing the call by `tool_call_id`.

**Anthropic**: Assistant message has `content` array with `tool_use` blocks (containing `id`, `name`, `input`). Tool results go in a single `user` message with `content` array of `tool_result` blocks (containing `tool_use_id`, `content`).

### 5. Extra headers for streaming

`agent_http_post_stream()` gains an `extra_headers` parameter (newline-separated string, same as `agent_http_post()` already supports). This allows passing `anthropic-version: 2023-06-01` for Anthropic requests.

### 6. Installer flow

After selecting claude/gemini/codex, a new sub-prompt:

```
How should aimee launch claude?
  a) Use the claude CLI directly (requires claude to be installed)
  b) Use Aimee's built-in chat (routes through the claude API)
```

Option b) prompts for endpoint (with provider-specific default), model, and API key. The key is stored in `~/.config/aimee/<provider>.key`. Config is saved with `use_builtin_cli: true`.

Option a) checks the native CLI is installed and sets `use_builtin_cli: false`.

### 7. Launch routing

```c
if (strcmp(provider, "openai") == 0 || cfg.use_builtin_cli) {
    cmd_chat(&ctx, 0, NULL);
    return 0;
}
execlp(provider, provider, NULL);
```

## Implementation

- `src/headers/config.h`: Add `int use_builtin_cli`
- `src/config.c`: Load/save `use_builtin_cli`
- `src/headers/agent_exec.h`: Add `extra_headers` to `agent_http_post_stream`
- `src/agent_http.c`: Implement `extra_headers` in streaming POST
- `src/cmd_chat.c`: Provider enum, Anthropic SSE parser, provider-specific request/auth/history
- `src/main.c`: Route to `cmd_chat` when `use_builtin_cli` is true
- `install.sh`: Sub-choice flow with provider-specific defaults

## Status

Complete.
