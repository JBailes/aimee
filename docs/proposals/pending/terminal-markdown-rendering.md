# Proposal: Terminal Markdown Renderer with Syntax Highlighting

## Problem

Aimee's CLI rendering (`render.c`) outputs agent responses as plain text. Code blocks, headings, emphasis, and links lose their formatting, making long agent responses hard to scan. Webchat has HTML rendering, but CLI users get a degraded experience.

The `soongenwong/claudecode` repo at `rust/crates/claw-cli/src/render.rs` implements markdown-to-terminal conversion with syntax-highlighted code blocks.

## Goals

- CLI output renders markdown with ANSI colors: bold headings, italic emphasis, colored syntax-highlighted code blocks.
- Language is detected from the fence tag and highlighted accordingly.
- Graceful degradation to plain text when terminal doesn't support color (piped output, `NO_COLOR`, dumb terminals).
- Webchat is unaffected — it already renders markdown via HTML.

## Approach

### Rendering Pipeline

```
Agent response (markdown)
  → Parse CommonMark nodes
  → Emit ANSI-escaped sequences:
      Heading:    bold + color
      Emphasis:   italic / underline
      Code span:  inverse background
      Code block: syntax-highlighted lines
      Link:       underline + blue + OSC 8 hyperlink
      List:       indented with bullet/number
```

### Syntax Highlighting

Keyword-based highlighter for common languages (C, Python, Rust, Go, JS, JSON, YAML, shell), or shell out to `bat --style=plain --color=always -l <lang>` when available.

### Changes

| File | Change |
|------|--------|
| `src/render.c` | Add markdown parsing and ANSI rendering functions |
| `src/render_highlight.c` (new) | Syntax highlighting: language detection, keyword coloring |
| `src/cli_main.c` | Use `render_markdown()` for agent text output |

### Color Detection

Respect `NO_COLOR`, `isatty()`, and `TERM=dumb`.

## Acceptance Criteria

- [ ] `# Heading` renders as bold colored text in CLI
- [ ] Code blocks with language tag render with syntax highlighting
- [ ] `*italic*`, `**bold**` render with ANSI italic/bold
- [ ] Piped output produces clean plain text without ANSI codes
- [ ] `NO_COLOR=1` disables all color

## Owner and Effort

- **Owner:** delegate (code)
- **Effort:** M (2-3 days)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Auto-detected from terminal capabilities.
- **Rollback:** `NO_COLOR=1` or revert.
- **Blast radius:** Broken ANSI sequences could corrupt terminal. Test with common terminals.

## Test Plan

- [ ] Unit tests: markdown parsing, ANSI generation, color detection
- [ ] Integration tests: render sample markdown, verify ANSI output
- [ ] Manual verification: run agent response through renderer in multiple terminals

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Heading + emphasis | P3 | S | Medium |
| Code block syntax highlighting | P3 | M | High — most impactful |
| Color detection + NO_COLOR | P3 | S | High — correctness |

## Source Reference

Implementation reference: `soongenwong/claudecode` at `rust/crates/claw-cli/src/render.rs`.
