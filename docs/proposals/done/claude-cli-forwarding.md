# Proposal: Forward Chat to Claude CLI

## Problem

When the provider is set to `claude`, aimee's built-in chat (both CLI and webchat) calls the Anthropic Messages API directly. This means aimee must manage its own conversation history, tool definitions, and tool execution loop — duplicating what Claude Code already handles natively. Users miss out on Claude Code's full capabilities: its own tools (Read, Edit, Bash, Grep, Glob, Agent, etc.), hooks, MCP servers, CLAUDE.md context, session persistence, and permission system.

The user expects that `aimee chat` and the webchat should behave like a frontend to Claude Code when the provider is `claude`, forwarding messages and streaming responses back.

## Goals

- When provider is `claude`, forward user messages to the `claude` CLI process instead of calling the API directly.
- Stream Claude's responses back in real-time (text deltas, tool calls, tool results).
- Each session (webchat or CLI) is isolated — separate Claude conversations.
- Webchat sessions persist across messages via `--resume <session_id>`.
- `/clear` in CLI chat resets the Claude session.
- Other providers (openai, gemini, codex) are unaffected.

## Approach

### 1. New provider type: `PROVIDER_CLAUDE`

Add `WC_PROVIDER_CLAUDE` and `CHAT_PROVIDER_CLAUDE` to the provider enums in `webchat.c` and `cmd_chat.c`. When `config.provider` is `"claude"`, set this provider and skip API key resolution, tool definition building, and conversation history management.

### 2. Claude CLI invocation

For each user message, spawn:

```
claude -p --output-format stream-json --include-partial-messages [--resume <session_id>] < message
```

The message is piped via stdin. The `--resume` flag is used on subsequent messages to continue the same conversation. The session ID is captured from the `result` event at the end of each response.

### 3. Stream-JSON parsing

Read stdout line by line. Each line is a JSON object:

- `{"type":"assistant",...}` — partial messages containing accumulated text and tool_use blocks. Compute text deltas by tracking previous text length.
- `{"type":"tool_result",...}` — tool execution results. Reset text tracking for the next turn.
- `{"type":"result","session_id":"..."}` — final result with session ID for resumption.

### 4. Webchat integration

In the `/api/chat/send` handler, when `session->provider == WC_PROVIDER_CLAUDE`, call `wc_chat_via_claude()` which sends SSE events (`text`, `tool_start`, `tool_result`, `done`) matching the existing frontend protocol.

### 5. CLI integration

In `cmd_chat`, the Claude provider gets its own REPL loop that calls `chat_via_claude()` per message, printing streamed text to stdout with ANSI formatting and tool call indicators to stderr.

### 6. Session isolation

- Each webchat session has its own `claude_session_id` field (256 bytes).
- New sessions start without `--resume`, creating a fresh Claude conversation.
- The session ID is captured from the first `result` event and reused for subsequent messages.
- CLI `/clear` zeros the session ID, starting a new conversation.

## Files changed

- `src/webchat.c` — added `WC_PROVIDER_CLAUDE`, `claude_session_id` to session struct, `wc_chat_via_claude()` function, early return in `session_init_chat` for claude provider, branch in chat handler
- `src/cmd_chat.c` — added `CHAT_PROVIDER_CLAUDE`, `claude_session_id` to state struct, `chat_via_claude()` function, dedicated REPL loop for claude provider
