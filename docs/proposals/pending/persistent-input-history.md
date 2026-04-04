# Proposal: Persistent Input History for Chat and Webchat

## Problem

Aimee's `chat` command has no persistent input history. When the user exits and re-enters chat, previous prompts are gone. Users must retype or copy-paste commands they've used before. There is no up-arrow recall, no history search, and no cross-session persistence.

Mistral-vibe implements a `HistoryManager` with JSON-line file storage, up/down navigation, duplicate suppression, and a configurable max entry limit (default 100).

## Goals

- Chat input history persists across sessions in both CLI and webchat.
- Up/down arrow keys navigate history in the CLI.
- Webchat provides a searchable history dropdown.
- Duplicate consecutive entries are suppressed.
- Slash commands (starting with `/`) are excluded from history.
- History is capped at a configurable max (default 200 entries).

## Approach

### CLI

Store history in `~/.aimee/chat_history.jsonl` (one JSON-encoded line per entry). On CLI chat startup, load history into a ring buffer. Wire up-arrow/down-arrow to navigate. New entries are appended on submit.

The existing `cmd_chat.c` reads input via `fgets` or a simple line reader. Replace this with a minimal line editor that supports:
- Up/down arrow for history navigation
- Preserving the current (unsaved) input when navigating
- Ctrl+R for reverse history search (stretch goal)

### Webchat

Store history per-user in a dedicated `input_history` table keyed by principal. The webchat input box gets a history dropdown (triggered by up-arrow or a button) showing recent unique prompts.

### Changes

| File | Change |
|------|--------|
| `src/headers/history.h` | New: history types, load/save/navigate API |
| `src/history.c` | New: JSONL file storage, ring buffer, dedup, navigation |
| `src/cmd_chat.c` | Integrate history into chat input loop |
| `src/webchat.c` | Add history API endpoint (GET/POST) |
| `src/webchat_assets.c` | Add history dropdown to webchat input |

## Acceptance Criteria

- [ ] Chat history persists across CLI sessions
- [ ] Up/down arrows navigate history in CLI chat
- [ ] Current unsaved input is preserved when navigating history
- [ ] Duplicate consecutive entries are suppressed
- [ ] Slash commands excluded from history
- [ ] History capped at configurable max (default 200)
- [ ] Webchat shows history dropdown with recent prompts
- [ ] History file stored at `~/.aimee/chat_history.jsonl`

## Owner and Effort

- **Owner:** aimee
- **Effort:** S-M (1-2 days)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** History file created automatically on first use.
- **Rollback:** Delete history file. No other state affected.
- **Blast radius:** CLI chat input only. No impact on other commands.

## Test Plan

- [ ] Unit tests: history load, save, navigation, dedup, cap enforcement
- [ ] Integration tests: enter 5 prompts, exit, re-enter, verify up-arrow recall
- [ ] Manual verification: webchat history dropdown

## Operational Impact

- **Metrics:** None
- **Logging:** None
- **Alerts:** None
- **Disk/CPU/Memory:** Negligible (~20KB for 200 entries)

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Persistent Input History | P3 | S-M | Medium — frequent UX friction point |

## Trade-offs

**Alternative: Use readline/libedit.** Adds a library dependency but provides history, completion, and editing for free. Worth considering if chat input grows more complex.

**Known limitation:** Multi-line inputs are stored as a single escaped JSON string. Readability in the history file is not a goal — it's machine-parsed.
