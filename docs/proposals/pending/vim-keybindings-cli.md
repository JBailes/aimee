# Proposal: Line Editor with Vim Keybindings for CLI Chat

## Problem

Aimee's built-in chat reads input via basic `fgets()` or readline with no custom keybinding support. Users who are accustomed to vim-style editing (hjkl navigation, dd/yy, visual selection, `:` commands) must fall back to arrow keys and basic cursor movement. There is no command history navigation, no multi-line editing, and no tab completion.

The claw-code project implements a full line editor in `input.rs` with:
- **Five editing modes**: Plain, Insert, Normal, Visual, Command (full vim emulation)
- **History navigation**: Up/Down arrows cycle through previous inputs with backup/restore of current line
- **Tab completion**: Prefix-based cycling for slash commands
- **Multi-line input**: Proper cursor positioning across wrapped lines
- **Raw terminal mode**: Direct ANSI escape sequence handling for responsive editing
- **Graceful degradation**: Falls back to basic input when stdin isn't a terminal

Aimee's CLI chat has none of this — input is a single line with no history, no vim bindings, and no completion.

## Goals

- CLI chat supports vim-style navigation (hjkl in normal mode, i for insert, dd/yy/p, visual selection).
- Command history is preserved across turns within a session (up/down arrows).
- Tab completion works for `/` slash commands.
- Multi-line input is supported (e.g., pasting code blocks).
- The editor falls back to basic readline when stdin is not a TTY.

## Approach

### 1. Raw Terminal Mode

Use `tcgetattr`/`tcsetattr` to enter raw mode. Handle ANSI escape sequences for:
- Arrow keys → cursor movement
- Home/End → line boundaries
- Backspace/Delete
- Ctrl-C (cancel line), Ctrl-D (exit)

### 2. Vim Mode (Optional)

When `AIMEE_VIM_MODE=1` or configured in `aimee config`:
- Start in insert mode
- `Esc` → normal mode (hjkl, dd, yy, p, w, b, 0, $)
- `i`/`a`/`A` → back to insert mode
- `v` → visual selection mode
- `:q` → exit chat

### 3. History

Maintain a ring buffer of previous inputs (max 100). Up/Down arrows navigate. Current in-progress input is saved when entering history and restored when exiting.

### 4. Tab Completion

When the cursor is after `/`, collect matching slash command names and cycle through them on repeated Tab presses.

### 5. Multi-line Input

Detect unbalanced code fences (` ``` `) and continue reading lines until closed. Display a continuation prompt (`... `).

### Changes

| File | Change |
|------|--------|
| `src/cmd_chat.c` | Replace `fgets()` input loop with line editor |
| `src/text.c` (new section or file) | Implement `line_editor_t` with raw mode, vim bindings, history, completion |
| `src/headers/text.h` | Declare line editor API |
| `src/config.c` | Add `chat.vim_mode` config key |

## Acceptance Criteria

- [ ] Arrow keys move cursor within the input line
- [ ] Up/Down navigates command history within the session
- [ ] Tab after `/` cycles through matching slash commands
- [ ] With vim mode enabled, `Esc` enters normal mode, `hjkl` navigates, `dd` deletes line
- [ ] Pasting a multi-line code block is captured as a single input
- [ ] Non-TTY stdin falls back to line-at-a-time reading
- [ ] Ctrl-C cancels current input, Ctrl-D exits chat

## Owner and Effort

- **Owner:** aimee maintainer
- **Effort:** M (raw terminal handling is well-understood; vim mode adds surface area but not algorithmic complexity)
- **Dependencies:** Slash commands proposal (for tab completion targets)

## Rollout and Rollback

- **Rollout:** Basic editing (arrows, history, completion) enabled by default. Vim mode opt-in via config.
- **Rollback:** Revert to `fgets()` input loop.
- **Blast radius:** CLI chat only.

## Test Plan

- [ ] Unit tests: line editor operations (insert, delete, move, history navigation)
- [ ] Integration tests: scripted input sequences produce expected editor state
- [ ] Manual verification: vim mode navigation, multi-line paste, tab completion

## Operational Impact

- **Metrics:** None
- **Logging:** None
- **Alerts:** None
- **Disk/CPU/Memory:** Negligible — character-at-a-time processing

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Line editor + history | P2 | S | High — basic quality-of-life for CLI users |
| Vim keybindings | P3 | M | Medium — power user feature |
| Tab completion | P2 | S | High — discoverable slash commands |

## Trade-offs

- **linenoise/readline dependency** vs **custom implementation**: linenoise is tiny (~1000 lines) and battle-tested. A custom implementation avoids the dependency but duplicates work. Recommend starting with linenoise for history+basic editing, then layering vim mode on top.
- **Full vim emulation** vs **subset**: Full vim is unbounded. The useful subset for a chat input is: hjkl, iIaAoO, dd/yy/p, w/b/e/0/$, visual selection, and `:q`. This covers 95% of muscle-memory use cases.
