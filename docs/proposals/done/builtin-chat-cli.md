# Proposal: Built-in OpenAI-compatible Chat CLI

## Problem

Aimee currently requires an external AI CLI tool (Claude Code, Gemini CLI, Codex CLI) to be installed separately. When the user runs `aimee`, it execs the configured provider's CLI binary. This creates two issues:

1. **Dependency on external tools.** Users who want to use an OpenAI-compatible API (OpenAI, Groq, Together, Ollama, or any provider offering an OpenAI-compatible endpoint) have no native CLI option. They must install a third-party wrapper or use curl.

2. **No unified tool surface.** Aimee's guardrails, memory, and indexing are injected via hooks into external CLIs. But each CLI has different tool semantics, different hook formats, and different capabilities. A built-in CLI gives aimee full control over the tool definitions and execution environment.

## Goals

- `aimee` with `provider: openai` launches an interactive chat REPL directly, no external binary needed.
- The chat supports streaming responses via Server-Sent Events (SSE).
- The chat supports tool use (function calling) with the same tools aimee exposes to external CLIs: `bash`, `read_file`, `write_file`, `list_files`, `grep`, `git_status`, `git_log`, `git_diff`, `verify`, `env_get`, `test`.
- Multi-turn tool call loops work correctly (model calls tool, gets result, calls another tool, etc.).
- The installer prompts for endpoint, model, and API key when the user selects the OpenAI-compatible option.

## Approach

### 1. `cmd_chat.c`: Interactive REPL with streaming

A new command handler `cmd_chat()` implements the full chat loop:

- **Config loading**: Reads `openai_endpoint`, `openai_model`, `openai_key_cmd` from `~/.config/aimee/config.json`.
- **Auth resolution**: Runs `openai_key_cmd` via popen, falls back to `OPENAI_API_KEY` env var.
- **System prompt**: Injects working directory and tool usage instructions.
- **SSE streaming**: Uses `agent_http_post_stream()` with a write callback that parses SSE `data:` lines incrementally. Token-by-token output is printed as it arrives.
- **Tool call accumulation**: Streaming tool call deltas (id, name, arguments) are assembled across multiple SSE events into complete tool calls.
- **Tool execution**: Each tool call is dispatched via `dispatch_tool_call()` from `agent_tools.c`. Results are appended to conversation history as `tool` role messages.
- **REPL commands**: `/quit`, `/exit`, `/clear` (reset conversation), `/model <name>` (switch model at runtime).
- **Signal handling**: SIGINT (Ctrl+C) gracefully interrupts the current response without killing the process.
- **Line continuation**: Backslash at end of line allows multi-line input.

### 2. Tool definitions in OpenAI format

`build_tools_array()` in `agent_tools.c` generates the JSON tool definitions in OpenAI function-calling format. Each tool specifies name, description, and a JSON Schema for parameters. The same tool dispatch function is used by both the chat CLI and the delegate agent system.

### 3. Installer integration

The installer adds a fourth option to the primary CLI selection:

```
Choose your primary AI CLI
  1) Claude   (claude)
  2) Codex    (codex)
  3) Gemini   (gemini)
  4) OpenAI-compatible (any OpenAI-compatible API)
```

Selecting option 4 prompts for endpoint, model, and API key. The key is stored in `~/.config/aimee/openai.key` with mode 0600.

### 4. Launch routing

In `main.c`, when no subcommand is given:

```c
if (strcmp(provider, "openai") == 0) {
    cmd_chat(&ctx, 0, NULL);
    return 0;
}
execlp(provider, provider, NULL);
```

## Implementation

- `src/cmd_chat.c` (670 lines): Full chat REPL implementation
- `src/main.c`: Launch routing for `provider == "openai"`
- `install.sh`: Option 4 with endpoint/model/key prompts
- `src/headers/config.h`: `openai_endpoint`, `openai_model`, `openai_key_cmd` fields
- `src/config.c`: Load/save OpenAI config fields

## Status

Complete. Shipped in commit `009d242`.
