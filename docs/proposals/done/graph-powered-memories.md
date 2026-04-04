# Proposal: Graph-Powered Related Memory Retrieval

## Problem

The memory graph system (`memory_graph.c`) extracts co-edited file edges and co-discussed term edges into the `entity_edges` table during window compaction. These edges are never queried for retrieval. The graph represents learned associations (files that change together, concepts that appear together) but this knowledge is wasted. Context assembly (`memory_context.c`) retrieves memories independently without considering relationships. When an agent asks about "authentication," it should also see related memories about "session tokens" and "CSRF protection" if those concepts have co-discussion edges.

## Goals

- Graph edges power related-memory expansion during retrieval
- Co-edited file edges improve blast-radius estimation
- Context assembly includes graph-adjacent memories
- Graph quality improves automatically as more sessions run

## Approach

1. **Add `memory_graph_related()` function** in `memory_graph.c`: given seed memory IDs, walk `co_discussed` edges (1 hop) to find related memories. Score by edge weight. Return top-K results not already in the seed set.
2. **Expand `memory_assemble_context()`** in `memory_context.c`: after initial retrieval returns top-K, call `memory_graph_related()` with those IDs as seeds. Add the top 4 graph-adjacent memories to context.
3. **Blast-radius expansion**: use `co_edited` edges to expand the affected file set. If file A has `co_edited` edges to B and C with weight > 3, include them in the result.
4. **Edge weight normalization** during extraction: normalize so the max weight per edge type is 1.0.
5. **Graph pruning** during maintenance: prune edges where both endpoints have expired or been demoted below L1.

### Changes

| File | Change |
|------|--------|
| `src/memory_graph.c` | Add `memory_graph_related()` for 1-hop edge walking; add `memory_graph_prune()` for expired edge cleanup |
| `src/memory_context.c` | Call `memory_graph_related()` after initial retrieval to expand context with graph-adjacent memories |
| `src/index.c` | Use `co_edited` edges to expand blast-radius file set |
| `src/memory_promote.c` | Invoke graph pruning during maintenance cycle |
| `src/headers/memory.h` | Declare `memory_graph_related()` and `memory_graph_prune()` |

## Acceptance Criteria

- [ ] `memory_graph_related()` returns related memory IDs scored by edge weight for a given seed set
- [ ] Context assembly includes up to 4 graph-adjacent memories alongside primary results
- [ ] Blast-radius estimation includes co-edited files with edge weight > 3
- [ ] Edge weights are normalized per edge type (max 1.0) during extraction
- [ ] Graph pruning removes edges where both endpoints are expired or below L1
- [ ] Graph expansion adds at most 4 memories and does not remove any primary results
- [ ] All new functions have corresponding unit tests

## Owner and Effort

- **Owner:** aimee
- **Effort:** M (medium)
- **Dependencies:** None (uses existing `entity_edges` table)

## Rollout and Rollback

- **Rollout:** Standard commit merge. No migration needed; reads existing `entity_edges` table.
- **Rollback:** Revert commit. `entity_edges` table is unchanged. Falls back to non-graph retrieval.
- **Blast radius:** Low. Graph expansion adds memories but does not remove any. Worst case: slightly larger context windows.

## Test Plan

- [ ] Unit tests: graph walking with known edges returns correct related IDs and scores
- [ ] Unit tests: weight normalization produces max 1.0 per edge type
- [ ] Integration tests: create memories with co-discussion edges, verify graph expansion appears in assembled context
- [ ] Integration tests: blast-radius expansion with `co_edited` edges includes related files
- [ ] Integration tests: graph pruning removes edges with expired endpoints
- [ ] Failure injection: empty graph returns gracefully with no expansion
- [ ] Manual verification: run a session, compact windows, query context for a term, confirm related terms appear

## Operational Impact

- **Metrics:** None new. Existing context assembly metrics cover expanded results.
- **Logging:** Debug-level log for graph expansion count per context assembly call.
- **Alerts:** None new.
- **Disk/CPU/Memory:** One graph query per context assembly (~1-2ms for typical graph sizes). Graph pruning during maintenance adds ~10ms. No new tables or schema changes.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Graph-powered related memory retrieval | P2 | M | Improves recall for related concepts without manual linking |

## Trade-offs

**Why only 1-hop?** Multi-hop walks produce increasingly noisy results. The association between "authentication" and "session tokens" (1-hop) is strong, but the transitive link to "database connection pooling" (2-hop) is unlikely to be relevant. The top-4 cap prevents graph expansion from overwhelming primary results.

**Why normalize weights?** Without normalization, heavily-discussed pairs dominate the ranking. A pair mentioned 50 times would always outrank a pair mentioned 5 times, even when the latter is more relevant to the current query. Normalization ensures edge weights reflect relative importance within their type.

**Why prune only when both endpoints are expired?** If one endpoint is still active, the edge may still be useful for future retrieval. Pruning only when both sides are gone keeps the graph useful while preventing unbounded growth.
