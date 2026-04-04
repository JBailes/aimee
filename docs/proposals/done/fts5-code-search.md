# Proposal: FTS5 Full-Text Code Search

## Problem

Currently, searching across the codebase relies on raw `grep` execution via a shell tool. This is slow in large repositories, consumes significant agent tokens when returning full file content to the context, and doesn't allow for ranked relevance. The agent often has to speculatively read many files to find a specific symbol or pattern that `grep` didn't quite pinpoint.

## Goals

- Provide near-instant full-text search across all indexed code files.
- Reduce token usage by providing high-quality snippets instead of requiring the agent to read entire files.
- Improve search relevance using SQLite's FTS5 ranking (BM25).
- Enable cross-project searching within a workspace.

## Approach

Implement a persistent full-text index using SQLite FTS5.

1.  **Schema Changes**:
    *   Add a `file_contents` table to store the raw text of files (needed for FTS5 `content` option).
    *   Create an `code_fts` virtual table using the FTS5 module.
    *   Implement database triggers to keep `code_fts` in sync with `file_contents` automatically.
2.  **Indexing Logic**:
    *   Update the `index_scan_project` and `index_scan_single_file` logic in `src/index.c` to populate the `file_contents` table whenever a file is scanned or updated.
3.  **New Agent Tool**:
    *   Implement a `code_search(query, project)` tool.
    *   The tool will use the `snippet()` function to return relevant context around matches.
    *   Results will be ranked by relevance and limited to the top 50 hits.

### Changes

| File | Change |
|------|--------|
| `src/db.c` | Add migration 37 for `file_contents` and `code_fts`. Register `code_search` tool. |
| `src/index.c` | Update `replace_file_data` to upsert into `file_contents`. |
| `src/agent_tools.c` | Implement `tool_code_search`. |
| `src/headers/agent_tools.h` | Export `tool_code_search`. |

## Acceptance Criteria

- [ ] `code_search` tool is available to agents.
- [ ] Searching for a unique string in a large project returns the correct file and a 20-word snippet in < 10ms.
- [ ] Index stays updated when files are modified via `write_file`.
- [ ] Performance benchmarks show no significant regression in project scanning time.

## Owner and Effort

- **Owner:** Backend Engineer
- **Effort:** M (3 days)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Automatic migration on next startup.
- **Rollback:** Revert schema changes; `code_search` tool will fail gracefully or be removed from the registry.
- **Blast radius:** Moderate. Large repositories will increase the SQLite database size significantly (roughly 1.5x - 2x the source code size).

## Test Plan

- [ ] Unit tests: Verify FTS5 triggers correctly update the index on file changes.
- [ ] Integration tests: Verify the agent can successfully find a symbol using `code_search` that it previously had to `grep` for.
- [ ] Performance: Measure database size growth and query latency on a 1M+ line repository.

## Operational Impact

- **Metrics:** `code_search_hits`, `code_search_latency_ms`.
- **Disk/CPU/Memory:** SQLite database size will grow significantly. Memory use during indexing will increase slightly as file content is buffered for the `INSERT`.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| FTS5 Search | P2 | M | High (Productivity & Speed) |

## Trade-offs

- **Database Size**: Storing full file content in SQLite doubles the storage requirement for indexed projects.
- **Scanning Overhead**: Extra `INSERT` overhead during the initial scan.
