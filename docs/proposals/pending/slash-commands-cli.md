# Proposal: Slash Commands for Built-in CLI Chat and Webchat

## Problem

Aimee's built-in chat (`cmd_chat.c`) and webchat (`webchat.c`) have no in-session commands. Once a chat session starts, the user can only type messages to the LLM or Ctrl-C/close tab to exit. There is no way to:
- Check session status (turns, tokens used, model) without exiting
- Switch models mid-conversation
- Compact history when approaching context limits
- View or export conversation history
- Run git operations without leaving chat
- Clear the session and start fresh

The claw-code project implements a comprehensive slash command system with 25+ commands covering session management (`/status`, `/clear`, `/resume`, `/export`), workspace operations (`/diff`, `/branch`, `/commit`, `/pr`), configuration (`/model`, `/permissions`, `/config`), and discovery (`/teleport`, `/agents`, `/skills`). Commands are dispatched from the REPL's `handle_submission()` before the input reaches the LLM.

Aimee already has a rich command system via `cmd_table.c` (50+ commands), but these are only accessible from the shell. None of them are available inside a chat session — not in the CLI, and not in the webchat.

## Goals

- Users can type `/command` during a chat session (CLI or webchat) to run session-level operations.
- Core commands: `/status`, `/model`, `/compact`, `/clear`, `/export`, `/help`, `/cost`, `/diff`, `/quit`.
- Commands execute immediately and return control to the chat prompt.
- The command set is extensible (new commands can be registered without modifying the dispatcher).
- CLI: Tab-completion for command names works in the REPL.
- Webchat: `/` prefix in the chat input is intercepted client-side and sent as a command request, not as a message to the LLM.

## Approach

### 1. Command Registry

Define a static table of slash commands with name, handler function, and help text:

```c
typedef int (*slash_cmd_fn)(chat_state_t *state, const char *args);

struct slash_cmd {
    const char *name;
    const char *help;
    slash_cmd_fn handler;
};
```

### 2. Dispatcher

In the chat input loop (`cmd_chat.c`), check if input starts with `/`. If so, dispatch to the matching handler instead of sending to the LLM:

```c
if (input[0] == '/') {
    int handled = dispatch_slash_command(state, input + 1);
    if (handled) continue;
    /* Unknown command: warn and continue */
}
```

### 3. Core Commands

| Command | Action |
|---------|--------|
| `/help` | List available slash commands |
| `/status` | Show turn count, model, provider, token usage |
| `/model [name]` | Display or switch model mid-session |
| `/cost` | Show estimated token cost for session |
| `/compact` | Summarize old messages to reduce context |
| `/clear` | Reset conversation history |
| `/export [file]` | Write conversation to file (JSON or markdown) |
| `/diff` | Show `git diff` for current workspace |
| `/quit` | Exit chat session |

### 4. Bridge to Existing Commands

For operations that map to existing aimee commands (e.g., `/diff` → `aimee git diff-summary`), the slash handler calls the server RPC directly rather than reimplementing logic.

### 5. Webchat Integration

In `webchat.c`, intercept messages starting with `/` in `POST /api/chat/send` (or the new `POST /api/sessions/{id}/message`). Instead of forwarding to the LLM:
1. Parse the command name and arguments
2. Execute the same handler used by the CLI
3. Return the result as a `command_result` SSE event (or JSON response)
4. The browser-side JS renders command results differently from assistant messages (e.g., monospace, dimmed, no markdown rendering)

The webchat input box should show a command autocomplete dropdown when the user types `/`.

### Changes

| File | Change |
|------|--------|
| `src/cmd_chat.c` | Add slash command dispatcher, registry, and handlers |
| `src/webchat.c` | Intercept `/` commands in message handler; return results via SSE |
| `src/headers/commands.h` | Declare shared `slash_cmd` struct and registry |

## Acceptance Criteria

- [ ] `/help` in chat session lists available commands
- [ ] `/status` shows turn count, model name, and token usage
- [ ] `/model claude-sonnet-4-6` switches model for subsequent turns
- [ ] `/compact` reduces message count and prints summary
- [ ] `/export chat.md` writes conversation as markdown
- [ ] Unknown `/foo` prints "Unknown command: foo" and continues (not sent to LLM)
- [ ] **CLI**: Tab-completion on `/` prefix shows available commands (if line editing supports it)
- [ ] **Webchat**: `/help` in webchat input returns command list rendered in chat
- [ ] **Webchat**: `/status` in webchat returns session info displayed as a formatted card
- [ ] **Webchat**: `/` prefix shows autocomplete dropdown in input box

## Owner and Effort

- **Owner:** aimee maintainer
- **Effort:** S (static dispatch table + handlers calling existing infrastructure)
- **Dependencies:** None (streaming-repl-upgrade proposal is complementary but independent)

## Rollout and Rollback

- **Rollout:** Available immediately in `aimee chat` sessions. No config needed.
- **Rollback:** Remove dispatcher check in input loop.
- **Blast radius:** Only affects `aimee chat` and webchat session handling. No impact on delegates or server internals.

## Test Plan

- [ ] Unit tests: dispatcher routes known commands, rejects unknown
- [ ] Integration tests: `/status` returns valid JSON with expected fields
- [ ] Manual verification: use `/help`, `/model`, `/compact` in a real session

## Operational Impact

- **Metrics:** None
- **Logging:** DEBUG for command dispatch
- **Alerts:** None
- **Disk/CPU/Memory:** Negligible

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Slash commands | P1 | S | High — low effort, high UX improvement for power users |

## Trade-offs

- **Expose all aimee commands** vs **curated subset**: Exposing everything would be overwhelming and some commands don't make sense mid-chat. A curated set of ~10 commands covers the common needs. Users can always `!aimee <cmd>` for the rest.
- **Plugin-extensible commands** vs **static table**: Extensibility adds complexity. Start with static table; add registration API later if demand materializes.
