# Proposal: Embedding-Based Memory Retrieval

## Problem

All memory retrieval is term-based: FTS5 full-text search with graph boost (`memory.c:426-740`, `memory_graph.c:216-275`). This works well for keyword matches but fails when:

1. **Semantic similarity without keyword overlap:** "PostgreSQL certificate authentication" won't match a memory about "database mTLS client certs" despite being the same concept.
2. **Paraphrase detection:** "Deploy failed because the service couldn't reach the database" won't match "DB connectivity issue caused deployment rollback."
3. **Context assembly relevance:** Task-aware context assembly (separate proposal) uses term overlap, which misses semantically related memories.

The current system compensates with the entity graph (co-discussed terms boost), but this only helps when terms were historically discussed together — not for novel associations.

## Goals

- Memory search supports semantic similarity in addition to keyword matching.
- Context assembly can use embedding similarity for relevance scoring.
- The embedding layer is optional — the system degrades gracefully to FTS5 when unavailable.

## Approach

### 1. Embedding Storage

Add an `embeddings` table:

```sql
CREATE TABLE memory_embeddings (
    memory_id INTEGER PRIMARY KEY REFERENCES memories(id) ON DELETE CASCADE,
    embedding BLOB NOT NULL,          -- float32 array, serialized
    model TEXT NOT NULL DEFAULT '',    -- model identifier for versioning
    created_at TEXT NOT NULL
);
```

### 2. Embedding Generation

Generate embeddings at memory insertion time via a configurable external command:

```c
/* Config: embedding_command = "python3 /path/to/embed.py" */
/* Invoked as: echo "memory content" | embedding_command */
/* Returns: JSON array of floats on stdout */
```

This keeps aimee dependency-free — no compiled-in ML libraries. The embedding provider is user-configured:
- Local: `ollama embed` with a small model (nomic-embed-text, 137M params)
- API: `curl` wrapper around OpenAI/Anthropic/Voyage embeddings
- None: skip embedding, fall back to FTS5

### 3. Similarity Search

Add `memory_search_semantic()`:

```c
int memory_search_semantic(sqlite3 *db, const char *query, 
                           search_result_t *out, int max);
```

1. Embed the query using the same command
2. Compute cosine similarity against stored embeddings
3. Return top-K results above a threshold (0.7)

For efficiency, scan only L1/L2 memories (skip L0 scratch and L3 historical). With <1000 memories, brute-force cosine similarity is fast enough (<10ms).

### 4. Hybrid Search

Combine FTS5 and embedding scores in `memory_search()`:

```
final_score = alpha * fts_score + (1 - alpha) * embedding_score + graph_boost

alpha = 0.5 (default, configurable)
```

If embeddings are unavailable (no command configured, or command fails), `alpha = 1.0` (pure FTS5, current behavior).

### 5. Context Assembly Integration

If the task-aware context assembly proposal lands, embedding similarity can replace or augment term overlap in relevance scoring.

### Changes

| File | Change |
|------|--------|
| `src/db.c` | New migration: `memory_embeddings` table |
| `src/memory.c` | `memory_embed()`, `memory_search_semantic()`, hybrid scoring in `memory_search()` |
| `src/headers/aimee.h` | Embedding config fields, `EMBED_SIMILARITY_THRESHOLD 0.7`, `EMBED_ALPHA 0.5` |
| `src/config.c` | `embedding_command` config key |

## Acceptance Criteria

- [ ] `memory_embed()` calls external command and stores embedding blob
- [ ] `memory_search_semantic()` returns semantically similar memories
- [ ] Hybrid search combines FTS5 and embedding scores
- [ ] System degrades gracefully when embedding command is not configured (pure FTS5)
- [ ] System degrades gracefully when embedding command fails (logs warning, falls back)
- [ ] Brute-force similarity search completes in <50ms for 1000 memories

## Owner and Effort

- **Owner:** TBD
- **Effort:** L
- **Dependencies:** External embedding provider (user-configured, not bundled)

## Rollout and Rollback

- **Rollout:** Migration creates table. Embeddings only generated when `embedding_command` is configured. Existing memories can be batch-embedded via `aimee memory embed --all`.
- **Rollback:** Revert commit. Empty embeddings table remains. Search reverts to pure FTS5.
- **Blast radius:** Optional feature. No impact when unconfigured. When configured, search results may change order.

## Test Plan

- [ ] Unit test: `memory_embed()` stores and retrieves embedding blob
- [ ] Unit test: cosine similarity computation is correct (known vectors)
- [ ] Unit test: hybrid scoring with alpha=0.5 blends FTS5 and embedding
- [ ] Unit test: missing embedding command falls back to FTS5 (alpha=1.0)
- [ ] Unit test: failed embedding command logs warning and continues
- [ ] Integration test: embed memories, search semantically, verify relevance
- [ ] Benchmark: 1000 memories, brute-force similarity in <50ms

## Operational Impact

- **Metrics:** Embedding generation time, search latency with/without embeddings.
- **Logging:** Embedding command invocation logged at debug. Fallback to FTS5 logged as warning.
- **Alerts:** None.
- **Disk/CPU/Memory:** ~4KB per memory (1024-dim float32). 1000 memories = ~4MB. Embedding generation adds ~100ms per insertion (external command). Search adds ~10ms (brute-force cosine).

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Embedding storage + generation | P3 | M | Foundation |
| Semantic search | P3 | M | Better retrieval |
| Hybrid scoring | P3 | S | Combines strengths |
| Context assembly integration | P3 | S | Depends on task-aware proposal |

## Trade-offs

**Why an external command instead of a compiled-in library?** aimee is a C tool. Embedding libraries (ONNX, llama.cpp) add significant compile-time and runtime dependencies. An external command is zero-dependency, user-configurable, and swappable. The 100ms overhead per insertion is acceptable for a non-real-time operation.

**Why brute-force instead of ANN (approximate nearest neighbors)?** With <1000 memories, brute-force cosine similarity is fast enough. ANN indices (HNSW, IVF) add complexity and are only justified at 10K+ vectors. If the memory corpus grows, a follow-up can add SQLite-vec or similar.

**Why not prioritize this?** FTS5 + graph boost handles the common case well. Semantic search primarily helps with novel associations and paraphrases — real but infrequent. The task-aware context assembly proposal (P1) delivers more impact with less cost.
