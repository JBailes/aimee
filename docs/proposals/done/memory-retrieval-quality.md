# Proposal: Memory Retrieval Quality Loop with Offline Evaluation Harness

## Problem

Memory retrieval (`memory_search()` at `memory.c:442-505`) uses a hybrid
approach: keyword matching via window-term JOIN queries, optionally blended with
semantic embeddings (`memory_search_semantic()` at `memory.c:1055`,
`EMBED_ALPHA = 0.5`). Retrieval quality is currently evaluated by manual spot
checks — there is no systematic way to measure whether changes to ranking,
compaction, or embedding parameters improve or degrade relevance.

Compaction rules (`memory_context.c:35-90`) prune terms on fixed schedules (30
days → summary, 90 days → fact) with L2-linked windows getting 2x terms. These
thresholds are intuition-based with no data supporting the specific values.

## Goals

- Measurable retrieval relevance scores via automated evaluation.
- Regression detection when ranking, compaction, or embedding parameters change.
- Data-driven tuning of compaction thresholds and blend weights.

## Approach

### 1. Evaluation corpus

Build a golden test set of (query, expected_results, relevance_scores) tuples.
Sources:
- Extract from existing memory databases (anonymized).
- Hand-label relevance for ~100 representative queries across memory tiers.
- Include edge cases: cross-tier retrieval, compacted memories, stale windows.

Store as a JSON file in `tests/eval/memory_retrieval_corpus.json`.

### 2. Evaluation metrics

Implement standard IR metrics:
- **MRR (Mean Reciprocal Rank):** Position of first relevant result.
- **NDCG@k (Normalized Discounted Cumulative Gain):** Ranking quality at k=5, k=10.
- **Recall@k:** Fraction of relevant results in top k.

### 3. Eval harness

`aimee eval memory-retrieval`:
1. Load eval corpus
2. For each query, run `memory_search()` and `memory_search_semantic()`
3. Compare results against golden labels
4. Output scorecard: MRR, NDCG@5, NDCG@10, Recall@5, Recall@10
5. Compare against baseline (stored in `tests/eval/memory_retrieval_baseline.json`)

### 4. CI integration

Add eval run to CI. Fail if any metric drops >5% from baseline. Store new
baseline when improvements are intentional.

### Changes

| File | Change |
|------|--------|
| `src/agent_eval.c` | Add memory retrieval eval mode |
| `src/cmd_core.c` | Add `aimee eval memory-retrieval` subcommand |
| `tests/eval/memory_retrieval_corpus.json` | Golden test set |
| `tests/eval/memory_retrieval_baseline.json` | Baseline scores |

## Acceptance Criteria

- [ ] Eval corpus contains ≥100 labeled query-result pairs
- [ ] `aimee eval memory-retrieval` outputs MRR, NDCG@5, NDCG@10, Recall@5, Recall@10
- [ ] Baseline comparison detects regressions >5%
- [ ] CI runs eval on every PR touching `memory.c`, `memory_context.c`, or `memory_advanced.c`

## Owner and Effort

- **Owner:** TBD
- **Effort:** M
- **Dependencies:** Existing `aimee eval` framework (`agent_eval.c`)

## Rollout and Rollback

- **Rollout:** Eval harness ships with binary. Corpus is a test artifact.
- **Rollback:** Revert commit. No runtime impact.
- **Blast radius:** None — eval is read-only and offline.

## Test Plan

- [ ] Unit test: MRR/NDCG/Recall calculations match known-good examples
- [ ] Integration test: eval harness runs against a small synthetic corpus
- [ ] Manual: verify scorecard output is interpretable and actionable

## Operational Impact

- **Metrics:** Eval scores tracked over time in CI artifacts.
- **Logging:** None (eval output only).
- **Alerts:** CI failure on regression.
- **Disk/CPU/Memory:** Eval run adds ~5-30s to CI depending on corpus size.

## Priority

P2 — enables data-driven tuning instead of intuition-based parameter changes.

## Trade-offs

**Why offline eval instead of online A/B testing?** aimee is single-user — there
is no traffic to A/B test against. Offline eval with a golden corpus is the
appropriate methodology.

**Why 100 queries minimum?** Fewer produces unreliable metrics. More is better but
requires significant labeling effort. 100 is a practical starting point.
