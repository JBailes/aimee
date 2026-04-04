# Proposal: Embedding-Powered Memory Retrieval Pipeline

## Problem

The memory system already has embedding infrastructure: `memory_embed()` stores vectors in the `memory_embeddings` table, `memory_search_semantic()` performs brute-force cosine similarity search, and constants are defined (`EMBED_MAX_DIM=1536`, `EMBED_SIMILARITY_THRESHOLD=0.7`, `EMBED_ALPHA=0.5` in `aimee.h`). However, embeddings are never generated automatically. `memory_embed()` must be called manually. The context assembly function `memory_assemble_context()` (`memory_context.c`) uses keyword matching only. An agent working on "authentication timeout" will not find a memory about "login session expiry" even though they describe the same problem.

## Goals

- Memories are automatically embedded on insertion and promotion
- Context assembly uses hybrid retrieval (keyword + semantic)
- Semantic search is fast enough for the tool-call hot path (<50ms)
- Embedding command is configurable (supports local models)

## Approach

### 1. Auto-embed on insert

In `memory_insert()` (`memory.c`), after successful insertion, if `config.embed_command` is set, call `memory_embed()` in the background (non-blocking, failure is non-fatal). This avoids adding 50-200ms of embedding latency to the tool-call path.

### 2. Auto-embed on promote

In `memory_run_maintenance()` (`memory_promote.c`), after promoting L1 to L2, embed the promoted memory if not already embedded.

### 3. Hybrid retrieval in context assembly

In `memory_assemble_context()`, run both FTS keyword search and `memory_search_semantic()`. Blend scores using `EMBED_ALPHA` (0.5 default):

```
final_score = alpha * fts_score + (1 - alpha) * cosine_similarity
```

Return top-K results by blended score.

### 4. Batch embedding command

Add `aimee memory embed-all` subcommand for bootstrapping existing memories that lack embeddings.

### 5. Performance

Brute-force scan is fine for <10K memories (<10ms). Log a warning at 10K suggesting an ANN index.

### Changes

| File | Change |
|------|--------|
| `src/memory.c` | Auto-embed after successful `memory_insert()` (background, non-fatal) |
| `src/memory_promote.c` | Auto-embed after L1-to-L2 promotion |
| `src/memory_context.c` | Hybrid retrieval blending FTS + semantic scores |
| `src/cmd_memory.c` | Add `embed-all` subcommand |
| `src/config.c` | Parse `embed_command` configuration |

## Acceptance Criteria

- [ ] Inserting a memory with `embed_command` configured creates an embedding row in `memory_embeddings` without blocking the caller
- [ ] Promoting a memory from L1 to L2 embeds it if not already embedded
- [ ] `memory_assemble_context()` returns semantically related memories (e.g., "auth timeout" matches "login session expiry")
- [ ] `aimee memory embed-all` embeds all un-embedded memories and reports progress
- [ ] Semantic search completes in <50ms for 1K memories
- [ ] If `embed_command` is unset or fails, all operations degrade gracefully to keyword-only

## Owner and Effort

- **Owner:** aimee
- **Effort:** L
- **Dependencies:** A working embedding command (e.g., via ollama or any stdin-text to stdout-JSON-float-array command)

## Rollout and Rollback

- **Rollout:** Set `embed_command` in config. Run `aimee memory embed-all` to bootstrap. New memories are embedded automatically from that point.
- **Rollback:** Revert commit. Existing `memory_embeddings` table is unaffected. Context assembly falls back to keyword-only.
- **Blast radius:** If the embedding command is slow or broken, memory insertion is unaffected (background, non-fatal). Context assembly falls back to keyword-only if no embeddings exist.

## Test Plan

- [ ] Unit tests: hybrid score blending with known FTS and cosine scores produces correct ranking
- [ ] Integration tests: insert a memory, verify an embedding row is created; insert two semantically similar memories with different keywords, verify context assembly finds both
- [ ] Failure injection: embedding command exits non-zero (verify insert still succeeds); embedding command hangs (verify insert is not blocked); corrupt embedding data (verify graceful fallback)
- [ ] Manual verification: insert memories about "auth timeout" and "login session expiry," query with either phrase, confirm both appear in assembled context

## Operational Impact

- **Metrics:** Count of embedded vs un-embedded memories; embedding command invocation latency
- **Logging:** Log embedding success/failure at DEBUG; log warning at 10K memories suggesting ANN index
- **Alerts:** None (embedding failures are non-fatal)
- **Disk/CPU/Memory:** One embedding command invocation per memory insert (background). Embedding storage: ~6KB per memory (1536 floats). For 1000 memories, ~6MB total.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Embedding-powered memory retrieval | P2 | L | High: enables semantic discovery of related memories across keyword boundaries |

## Trade-offs

**Brute-force vs ANN index:** For <10K memories, brute-force cosine similarity is faster than maintaining an index and avoids the complexity of an external ANN library. A warning at 10K nudges users toward an index if they reach that scale.

**Background embedding vs synchronous:** Embedding adds 50-200ms latency. Blocking insertion would slow the tool-call path. Background execution means a memory may be briefly un-embedded, but this is acceptable since keyword search still works.

**Fixed alpha vs learned weighting:** A fixed `EMBED_ALPHA=0.5` is simple and configurable. Learning optimal weights would require labeled relevance data that does not exist yet.
