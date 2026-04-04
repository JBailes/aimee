# Proposal: Memory Write Quality Gates

## Problem

Any caller can write any content to long-term memory without validation. The only checks at insertion time are deduplication (exact key match and trigram similarity). This means:

1. **Low-value memories accumulate.** Ephemeral observations ("file X has 200 lines") get stored as L1 facts and persist for 30 days, consuming context budget and adding retrieval noise.

2. **Secrets and PII can be persisted.** If the agent encounters a password, API key, or personal identifier in tool output and stores it as a fact, it persists in SQLite with no encryption or access control.

3. **Hallucinated or unverifiable claims get stored.** The agent may synthesize a "fact" from incomplete information. Without a source or confidence gate, it enters the memory system with the same standing as a verified observation.

4. **Contradictions with existing memory are only detected post-write.** Retroactive conflict detection (pending proposal) runs daily. A contradictory fact inserted now won't be flagged for up to 24 hours.

Evidence: `memory_insert()` in `memory.c:62` accepts any content string. The only rejection path is when dedup finds an exact match. No content validation, no sensitivity check, no minimum quality threshold.

## Goals

- Memories pass a quality gate before being persisted.
- Secrets, PII, and sensitive content are detected and blocked (or redacted) before write.
- Low-confidence or ephemeral content is routed to L0 scratch instead of L1+ long-term storage.
- Contradictions with existing high-confidence memories are detected at write time.

## Approach

### 1. Gate Pipeline

Insert a validation pipeline between the caller and `memory_insert()`:

```c
typedef enum {
    GATE_ACCEPT,      /* write as requested */
    GATE_DOWNGRADE,   /* write to L0 scratch instead of requested tier */
    GATE_REDACT,      /* write with sensitive content masked */
    GATE_REJECT       /* do not write */
} gate_result_t;

typedef struct {
    gate_result_t result;
    char reason[256];
    char redacted_content[2048]; /* only set if result == GATE_REDACT */
} gate_verdict_t;

int memory_gate_check(sqlite3 *db, const char *tier, const char *kind,
                      const char *key, const char *content,
                      double confidence, gate_verdict_t *verdict);
```

### 2. Sensitivity Gate

Pattern-match content against known sensitive patterns before write:

```c
static const char *sensitive_patterns[] = {
    /* API keys and tokens */
    "(?i)(api[_-]?key|token|secret|password|passwd|credential)\\s*[:=]\\s*\\S+",
    /* AWS keys */
    "AKIA[0-9A-Z]{16}",
    /* Private keys */
    "-----BEGIN (RSA |EC |DSA )?PRIVATE KEY-----",
    /* Common PII */
    "(?i)(social.security|ssn|date.of.birth|dob)\\s*[:=]\\s*\\S+",
    NULL
};
```

If matched:
- If the sensitive portion can be isolated: `GATE_REDACT` with the value replaced by `[REDACTED]`
- If the entire content is sensitive: `GATE_REJECT`

Provenance records the rejection reason: `action="gate_reject", details="sensitivity: api_key pattern"`.

### 3. Stability Gate

Route likely-ephemeral content to L0 scratch:

```c
/* Heuristics for ephemeral content */
static int is_ephemeral(const char *content) {
    /* Contains line counts, byte sizes, or file listings */
    if (regex_match(content, "\\b\\d+ (lines|bytes|files)\\b")) return 1;
    /* Contains timestamps that look like "right now" */
    if (regex_match(content, "\\b(just now|currently|right now|at the moment)\\b")) return 1;
    /* Very short content (<20 chars) with no decision or fact markers */
    if (strlen(content) < 20) return 1;
    return 0;
}
```

If the caller requests L1+ but the content looks ephemeral: `GATE_DOWNGRADE` to L0 scratch. The caller is informed via the verdict.

### 4. Conflict Gate

Before writing, check if any existing L2 memory with confidence >= 0.8 contradicts the new content:

```c
/* Quick contradiction check: same key, different content */
SELECT id, content, confidence FROM memories
WHERE key = ? AND tier = 'L2' AND confidence >= 0.8
  AND content != ?
LIMIT 1;
```

If found:
- If new memory has higher confidence: `GATE_ACCEPT` but log the conflict and decay the old memory
- If existing memory has higher confidence: `GATE_DOWNGRADE` to L1 (needs more evidence before overwriting L2)
- Record in `memory_conflicts` table for operator review

### 5. Source Gate

If the memory has no `source_session` or the confidence is above 0.9 but the content is not directly derived from tool output (heuristic: doesn't reference specific files, commands, or outputs), downgrade confidence to 0.7:

```c
/* High confidence requires evidence */
if (confidence > 0.9 && !has_evidence_markers(content))
    adjusted_confidence = 0.7;
```

This prevents the agent from asserting high-confidence "facts" without grounding.

### Changes

| File | Change |
|------|--------|
| `src/memory.c` | Add `memory_gate_check()`, call from `memory_insert()` |
| `src/headers/memory.h` | Gate types, verdict struct |
| `src/headers/aimee.h` | `GATE_CONFIDENCE_FLOOR 0.7` constant |
| `src/memory_promote.c` | Record gate rejections in provenance |

## Acceptance Criteria

- [ ] Content matching sensitive patterns is rejected or redacted before persistence
- [ ] Ephemeral content requested at L1+ is downgraded to L0 scratch
- [ ] Content conflicting with high-confidence L2 is handled (accept with decay or downgrade)
- [ ] High-confidence memories without evidence markers are capped at 0.7
- [ ] Gate verdicts are recorded in `memory_provenance` with reasons
- [ ] Existing insertion paths (maintenance, scan, manual) all pass through the gate
- [ ] Gate pipeline adds <5ms to insertion latency

## Owner and Effort

- **Owner:** TBD
- **Effort:** M
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Gate is inserted into `memory_insert()` call path. All callers automatically pass through it. No migration needed (no schema changes).
- **Rollback:** Revert commit. `memory_insert()` returns to ungated behavior.
- **Blast radius:** Some memories that previously would have been stored will now be rejected, redacted, or downgraded. This is intentional. Provenance logs capture every gate decision for debugging.

## Test Plan

- [ ] Unit test: content with "api_key=sk-abc123" is rejected
- [ ] Unit test: content with "password: hunter2" is redacted to "password: [REDACTED]"
- [ ] Unit test: "file has 200 lines" at L1 is downgraded to L0
- [ ] Unit test: content contradicting L2 confidence=0.9 is downgraded to L1
- [ ] Unit test: confidence=0.95 without evidence markers is capped at 0.7
- [ ] Unit test: legitimate fact passes all gates unchanged
- [ ] Integration test: gate verdicts appear in `memory_provenance`
- [ ] Benchmark: gate pipeline <5ms for typical content

## Operational Impact

- **Metrics:** Gate accept/reject/redact/downgrade counts in `aimee memory stats`.
- **Logging:** Each gate verdict logged at info level (accept) or warn level (reject/redact).
- **Alerts:** None.
- **Disk/CPU/Memory:** Regex matching per insertion. Negligible for typical content length (<2KB).

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Sensitivity gate | P1 | S | Security hygiene |
| Stability gate | P2 | S | Reduces noise |
| Conflict gate | P2 | S | Catches contradictions early |
| Source gate | P3 | S | Confidence calibration |

## Trade-offs

**Why regex for sensitivity instead of a proper secrets scanner?** aimee is a C tool with no external dependencies. Regex patterns catch the common cases (API keys, passwords, private keys) without adding a library. False positives are acceptable here — a gate rejection is not data loss, it's a write that doesn't happen. The operator can always insert manually.

**Why downgrade instead of reject for ephemeral content?** Rejecting outright is too aggressive — the caller may have legitimate reasons. Downgrading to L0 scratch preserves the content for the current session while preventing long-term pollution. If the content proves valuable, it can be promoted through the normal lifecycle.

**Why not gate on content length?** Long content isn't inherently low-quality. A detailed procedure or incident report may be 1KB+. Length-based gating would penalize exactly the memories that are most valuable.
