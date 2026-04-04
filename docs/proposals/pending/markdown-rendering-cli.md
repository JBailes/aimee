# Proposal: Markdown Rendering and Syntax Highlighting in CLI Chat and Webchat

## Problem

Aimee's built-in chat (`cmd_chat.c`) streams raw text to stdout with no formatting. When the LLM returns markdown (headings, code blocks, lists, tables), the user sees raw `#`, triple backticks, and `|` pipes. Code blocks have no syntax highlighting. Tool outputs are printed as plain text with no visual distinction from assistant prose.

Aimee's webchat (`webchat.c`) has the same problem on the server side — it streams raw text to the browser via SSE. The embedded HTML UI does basic `<pre>` rendering but has no markdown parsing, no syntax highlighting for code blocks, and no structured rendering of tables or lists. The webchat UI is a single embedded HTML string in `dashboard.c` and `webchat.c` with minimal CSS.

The claw-code project implements a full terminal markdown renderer in `render.rs` using `pulldown_cmark` for event-based markdown parsing and `syntect` for 24-bit syntax highlighting. Key features:
- **Color theme system**: Distinct colors for headings, emphasis, inline code, links, blockquotes
- **Syntax-highlighted code blocks**: Language-aware highlighting with background coloring
- **Table rendering**: Unicode box-drawing characters, auto-calculated column widths
- **Streaming-safe rendering**: `MarkdownStreamState` detects safe flush boundaries (complete paragraphs, closed code blocks) so partial markdown doesn't break formatting
- **Spinner animation**: Braille character spinner with color transitions

The duplicate terminal-only proposal should be folded into this one. The CLI and webchat rendering work share parsing rules, output semantics, and acceptance criteria; keeping them separate only encourages inconsistent formatting behavior.

## Goals

- **CLI**: Assistant markdown output is rendered with ANSI colors and formatting in the terminal.
- **CLI**: Code blocks show syntax highlighting for common languages (C, Rust, Python, shell, JSON, YAML).
- **CLI**: Tables render with box-drawing characters and aligned columns.
- **Both**: Streaming text is flushed at safe boundaries to avoid broken formatting mid-render.
- **CLI**: Formatting is disabled when stdout is not a TTY (piped output stays raw).
- **Webchat**: SSE events include structured content type hints (text, code block with language, table).
- **Webchat**: Browser-side rendering uses a lightweight markdown library (marked.js or similar) with syntax highlighting (highlight.js or Prism).

## Approach

### 1. Markdown-to-ANSI Renderer

Implement an event-driven markdown renderer in C. Use a lightweight markdown parser (cmark or a minimal hand-rolled one for the subset we need: headings, code blocks, emphasis, lists, tables, links).

Map markdown events to ANSI escape sequences:
- Headings: bold + cyan
- Code blocks: dim background + language-specific highlighting
- Inline code: green
- Bold/italic: ANSI bold/italic
- Tables: `│`, `─`, `┼` box-drawing

### 2. Syntax Highlighting

Embed a minimal keyword-based highlighter for C, Rust, Python, shell, JSON, YAML. This doesn't need to be a full parser — keyword coloring + string/comment detection covers 90% of the value. Alternatively, shell out to `bat --plain --color=always` if available, with fallback to no highlighting.

### 3. Streaming Boundary Detection

Track open/close state for code fences, emphasis markers, and table rows. Buffer text until a safe boundary is reached:
- End of paragraph (blank line)
- Close of code fence
- End of table row
- End of list item

Flush the buffer through the renderer at each boundary. Outside of structured blocks, flush every complete line.

### 4. Integration

Wire the renderer into `cmd_chat.c`'s text output path. When `isatty(STDOUT_FILENO)` is true, pipe text through the renderer. When false, output raw text (for piping to `jq`, files, etc.).

### 5. Webchat: Browser-Side Markdown Rendering

Add a lightweight JS markdown renderer to the embedded webchat HTML:
- Include `marked.min.js` (~40KB) inline or load from CDN for markdown parsing
- Include `highlight.min.js` (~30KB) for code block syntax highlighting
- Render each assistant message through the markdown pipeline before inserting into DOM
- Apply CSS styling consistent with the existing dark theme (`.card` backgrounds, color palette)
- Streaming: accumulate text deltas, re-render the current message on each delta (debounced to 100ms to avoid excessive DOM updates)

### Changes

| File | Change |
|------|--------|
| `src/render.c` | Add `render_markdown_ansi()` and streaming state tracker |
| `src/cmd_chat.c` | Route assistant text through markdown renderer when TTY |
| `src/webchat.c` | Embed marked.js + highlight.js in chat HTML; render assistant messages as markdown |
| `src/headers/render.h` | Declare markdown rendering API |

## Acceptance Criteria

- [ ] **CLI**: `aimee chat` renders `# Heading` as bold cyan text
- [ ] **CLI**: Code blocks with ` ```c ` show at minimum keyword highlighting
- [ ] **CLI**: Tables render with box-drawing characters and aligned columns
- [ ] **CLI**: Partial streaming doesn't produce broken ANSI sequences or split code fences
- [ ] **CLI**: `aimee chat | cat` outputs raw markdown (no ANSI escapes)
- [ ] **Webchat**: Assistant messages render as formatted HTML with headings, lists, and styled code blocks
- [ ] **Webchat**: Code blocks show syntax highlighting with language detection
- [ ] **Webchat**: Streaming updates re-render incrementally without visible flicker
- [ ] **Both**: Performance: rendering adds < 1ms per flush (CLI) / < 16ms per debounce (webchat)

## Owner and Effort

- **Owner:** aimee maintainer
- **Effort:** M (markdown subset is bounded; syntax highlighting is the hardest part)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** CLI: enabled by default when TTY detected, `--raw` flag to force plain output. Webchat: enabled for all sessions.
- **Rollback:** CLI: remove renderer call in `cmd_chat.c`, revert to raw `fwrite()`. Webchat: remove JS libraries from embedded HTML.
- **Blast radius:** Display only. No impact on server logic or delegates.

## Test Plan

- [ ] Unit tests: render known markdown strings, compare ANSI output against snapshots
- [ ] Integration tests: `aimee chat --output=text` piped through `cat` has no ANSI escapes
- [ ] Manual verification: visually confirm headings, code blocks, tables in terminal

## Operational Impact

- **Metrics:** None
- **Logging:** None
- **Alerts:** None
- **Disk/CPU/Memory:** Negligible — string processing only

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Markdown rendering | P2 | M | High — transforms CLI chat from raw text dump to readable output |

## Trade-offs

- **Full cmark library** vs **hand-rolled subset parser**: cmark is correct but adds a dependency. A subset parser handling headings/code/emphasis/lists/tables covers 95% of LLM output. Start with subset, migrate to cmark if edge cases accumulate.
- **`bat` for highlighting** vs **built-in**: `bat` produces excellent output but is an optional dependency. Built-in keyword highlighting is simpler and always available. Can support both with `bat` as preferred when present.
- **Full `pulldown-cmark` equivalence** is not a goal — aimee's C codebase doesn't need a full CommonMark implementation. The LLM output is predictable enough for a subset.
