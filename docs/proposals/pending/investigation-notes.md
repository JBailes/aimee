# Proposal: Investigation Notes

## Problem

When agents work on complex debugging or research tasks, they accumulate findings, hypotheses, dead ends, and conclusions. Today this knowledge either:
1. Lives only in the conversation context and is lost when the session ends
2. Gets saved as memory facts, which are meant for persistent cross-session knowledge and pollute the memory tier with ephemeral investigation details
3. Isn't captured at all — the agent solves the problem but the reasoning trail is gone

ayder-cli addresses this with a `create_note` tool that writes structured markdown notes to `.ayder/notes/`. Notes have titles, YAML frontmatter (date, tags), and markdown content. They're separate from memory (persistent facts) and tasks (work items) — they're investigation artifacts.

This is useful for aimee because:
- Delegates investigating bugs could document their findings in notes, making the trail reviewable
- Multi-delegate investigations could share notes as coordination artifacts
- Post-incident reviews benefit from captured investigation reasoning
- Notes complement memory: memory stores conclusions ("service X uses port 8443"), notes store the reasoning ("tried ports 443, 8080, 8443 — only 8443 responded because mTLS requires the SPIRE-issued cert")

Evidence:
- `memory.c` and `memory_advanced.c` store facts but have no concept of investigation artifacts
- Delegates produce verbose output in session logs but this is hard to review after the fact
- No structured way to capture "what I tried and why" during debugging

## Goals

- Agents can create structured markdown notes during investigations, capturing findings, hypotheses, and reasoning.
- Notes are stored per-project in `.aimee/notes/` with YAML frontmatter (title, date, tags).
- Notes are searchable by title, tags, and content.
- Notes are distinct from memory (persistent facts) and tasks (work items).
- Delegates can read notes created by other delegates in the same session or project.

## Approach

### 1. Note storage

Notes are markdown files in `.aimee/notes/`:

```
.aimee/notes/
  auth-middleware-investigation.md
  performance-regression-analysis.md
```

Each note has YAML frontmatter:

```markdown
---
title: "Auth middleware investigation"
date: 2026-04-04 14:30:00
tags: [debugging, auth, middleware]
author: delegate:review
---

## Findings

1. The middleware checks X.509 certs via SPIRE SVIDs
2. Port 8443 is the only mTLS-enabled port
3. The timeout was caused by cert rotation during the health check window

## Dead ends

- Checked nginx proxy config — not the issue
- Tried increasing timeout to 30s — symptom masked, not fixed
```

### 2. MCP tools

Add three MCP tools:

```c
// Create or append to a note
// tool: create_note
// params: title (string), content (string), tags (string, comma-separated, optional)
int tool_create_note(const char *title, const char *content, const char *tags);

// List notes, optionally filtered by tag
// tool: list_notes
// params: tag (string, optional), limit (int, optional, default 20)
int tool_list_notes(const char *tag, int limit);

// Search note content
// tool: search_notes
// params: query (string)
int tool_search_notes(const char *query);
```

### 3. Title-to-slug filename

Convert note titles to kebab-case slugs for filenames, truncated to 50 chars:
- `"Auth Middleware Investigation"` -> `auth-middleware-investigation.md`
- Creating a note with an existing slug appends to the existing note

### 4. System prompt integration

When notes exist in the project, include a brief mention in the agent system prompt:

```
Investigation notes are available in .aimee/notes/. Use create_note to document findings during investigations. Use search_notes to check for prior investigation context.
```

### Changes

| File | Change |
|------|--------|
| `src/notes.c` (new) | Note creation, listing, searching, slug generation, frontmatter parsing |
| `src/headers/notes.h` (new) | Public note API |
| `src/mcp_tools.c` | Register `create_note`, `list_notes`, `search_notes` tools |
| `src/agent_tools.c` | Add note tools to delegate tool sets |

## Acceptance Criteria

- [ ] `create_note` tool creates markdown files in `.aimee/notes/` with YAML frontmatter
- [ ] `list_notes` returns notes sorted by date, optionally filtered by tag
- [ ] `search_notes` finds notes by content substring match
- [ ] Creating a note with an existing title appends to the existing note
- [ ] Notes appear in MCP tool listings and are callable by delegates
- [ ] Note tools are available in delegate sessions

## Owner and Effort

- **Owner:** delegate (code)
- **Effort:** S (1-2 days)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Additive — no notes directory means tools return empty results. Note creation is explicit (tool call only).
- **Rollback:** Remove `.aimee/notes/` to clear notes. Remove tool registrations to disable.
- **Blast radius:** Zero — notes are passive markdown files with no effect on core behavior.

## Test Plan

- [ ] Unit tests: slug generation, frontmatter parsing, note creation, append-to-existing, list with tag filter, content search
- [ ] Integration tests: delegate creates a note, another delegate reads it
- [ ] Failure injection: invalid title characters, very long content, concurrent note creation
- [ ] Manual verification: `aimee delegate review "investigate auth timeout"`, verify notes appear in `.aimee/notes/`

## Operational Impact

- **Metrics:** `notes_created_total`, `notes_searched_total`
- **Logging:** Note creation at DEBUG
- **Alerts:** None
- **Disk/CPU/Memory:** Negligible — notes are small markdown files

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| create_note + storage | P2 | S | High — captures investigation reasoning |
| list_notes + search | P3 | S | Medium — discoverability |
| System prompt integration | P3 | S | Low — improves adoption |

## Trade-offs

- **Why not just use memory?** Memory is for persistent cross-session facts. Investigation notes are ephemeral artifacts — they document the process, not just the conclusion. Mixing them pollutes memory with noise. Notes can be reviewed and selectively promoted to memory after an investigation.
- **Why not use session logs?** Session logs capture everything including tool call noise, token counts, and system messages. Notes are curated artifacts the agent deliberately creates. They're far more readable for post-investigation review.
- **Why markdown with frontmatter?** It's human-readable, git-friendly, and grep-able. No database dependency. Matches aimee's existing `.aimee/` convention.

## Source Reference

Implementation reference: ayder-cli `src/ayder_cli/tools/builtins/notes.py` — `create_note()` function with YAML frontmatter, slug generation, and tag support.
