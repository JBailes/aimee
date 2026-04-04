# Proposal: Content Safety Gates & Retention Controls

## Problem

Memory persistence has no safety layer. The system will store anything the agent writes, including:

1. **Secrets in tool output.** If the agent reads a `.env` file or sees database credentials in a log, and records a "fact" about the database, the credentials may be embedded in the memory content.

2. **PII from user interactions.** Names, email addresses, or internal identifiers mentioned in conversation can persist as facts or preferences indefinitely.

3. **No retention policy.** All non-expired memories persist forever in SQLite. There is no mechanism for time-bounded retention, sensitivity classification, or operator-initiated purge by category.

4. **No "don't memorize" signal.** The agent has no way to mark content as ephemeral-only. If a user shares a temporary secret ("here's a one-time deploy token"), it may be recorded as a fact.

This is distinct from the memory write quality gates proposal, which focuses on content quality. This proposal addresses content *safety* — what should never be persisted, what should be persisted with controls, and what retention rules govern persistence.

Evidence: `memory_insert()` stores the raw `content` string. No scanning, no classification, no retention metadata. The `valid_until` field exists but is only used for temporal supersession, not retention policy.

## Goals

- Sensitive content (secrets, PII, credentials) is detected and blocked or redacted before persistence.
- Memories have a sensitivity classification that governs retention and retrieval.
- Operators can set retention policies per sensitivity class.
- A "don't memorize" signal prevents specific content from entering long-term memory.

## Approach

### 1. Sensitivity Classification

Add a `sensitivity` column to the `memories` table:

```sql
ALTER TABLE memories ADD COLUMN sensitivity TEXT NOT NULL DEFAULT 'normal';
/* Values: 'public', 'normal', 'sensitive', 'restricted' */
```

| Class | Retention | Retrieval | Examples |
|-------|-----------|-----------|---------|
| `public` | Indefinite | Always included | Architecture decisions, coding conventions |
| `normal` | Standard lifecycle | Standard retrieval | Facts, episodes, tasks |
| `sensitive` | Max 90 days, never in cross-workspace | Same-workspace only | Internal URLs, team-specific procedures |
| `restricted` | Max 7 days, encrypted at rest (future) | Explicit request only | Temporary credentials, deploy tokens |

### 2. Content Scanning

Scan content before write using pattern matching:

```c
typedef struct {
    const char *pattern;      /* regex */
    const char *class_name;   /* sensitivity class to assign */
    int action;               /* SCAN_BLOCK, SCAN_REDACT, SCAN_CLASSIFY */
} scan_rule_t;

static const scan_rule_t scan_rules[] = {
    /* Block: never persist */
    {"-----BEGIN.*PRIVATE KEY-----",           "restricted", SCAN_BLOCK},
    {"AKIA[0-9A-Z]{16}",                       "restricted", SCAN_BLOCK},

    /* Redact: persist with value masked */
    {"(?i)(password|passwd|secret)\\s*[:=]\\s*(\\S+)", "restricted", SCAN_REDACT},
    {"(?i)(api[_-]?key|token)\\s*[:=]\\s*(\\S+)",     "restricted", SCAN_REDACT},

    /* Classify: persist but mark sensitive */
    {"(?i)(\\b\\d{3}-\\d{2}-\\d{4}\\b)",       "restricted", SCAN_REDACT},  /* SSN */
    {"(?i)\\b[A-Z0-9._%+-]+@[A-Z0-9.-]+\\.[A-Z]{2,}\\b", "sensitive", SCAN_CLASSIFY},
    {"(?i)(10\\.\\d+\\.\\d+\\.\\d+|192\\.168\\.\\d+\\.\\d+)", "sensitive", SCAN_CLASSIFY},

    {NULL, NULL, 0}
};
```

### 3. Retention Enforcement

A maintenance job enforces retention policies:

```c
/* During memory_run_maintenance() */
void memory_enforce_retention(sqlite3 *db) {
    /* Restricted: expire after 7 days regardless of tier */
    "DELETE FROM memories WHERE sensitivity = 'restricted'"
    " AND created_at < datetime('now', '-7 days')";

    /* Sensitive: expire after 90 days regardless of tier */
    "DELETE FROM memories WHERE sensitivity = 'sensitive'"
    " AND created_at < datetime('now', '-90 days')";
}
```

Provenance records the forced expiry: `action="retention_expire", details="sensitivity=restricted, age=8d"`.

### 4. "Don't Memorize" Signal

Add a mechanism for callers to mark content as session-only:

```c
/* Insert with no_persist flag — stored as L0 scratch with valid_until = session end */
int memory_insert_ephemeral(sqlite3 *db, const char *key, const char *content,
                            const char *session_id);
```

This sets:
- `tier = 'L0'`
- `kind = 'scratch'`
- `sensitivity = 'restricted'`
- `valid_until = datetime('now', '+1 hour')`

The memory is available for the current session but cannot be promoted and will be cleaned up by retention enforcement.

### 5. Retrieval Filtering

Sensitive and restricted memories are excluded from cross-workspace retrieval:

```sql
/* In cross-workspace memory queries, add: */
AND sensitivity IN ('public', 'normal')
```

Restricted memories are only included when explicitly requested (future: via MCP tool parameter).

### 6. Audit Trail

All safety-related actions are logged to provenance:

- `action="scan_block", details="pattern: AKIA..., content preview: AKIA****"`
- `action="scan_redact", details="pattern: password=..., field: password"`
- `action="scan_classify", details="sensitivity: sensitive, pattern: email"`
- `action="retention_expire", details="sensitivity: restricted, age: 8d"`

### Changes

| File | Change |
|------|--------|
| `src/db.c` | Migration: `sensitivity` column on memories |
| `src/memory.c` | `memory_scan_content()`, content scanning before insert, `memory_insert_ephemeral()` |
| `src/memory_promote.c` | `memory_enforce_retention()`, call from maintenance |
| `src/memory_context.c` | Exclude restricted from cross-workspace retrieval |
| `src/headers/memory.h` | `scan_rule_t`, `SCAN_BLOCK/REDACT/CLASSIFY` |
| `src/headers/aimee.h` | Retention constants (`RETENTION_RESTRICTED_DAYS 7`, `RETENTION_SENSITIVE_DAYS 90`) |

## Acceptance Criteria

- [ ] Private keys and AWS access keys are blocked from persistence
- [ ] Passwords and API keys are redacted (value replaced with `[REDACTED]`)
- [ ] Email addresses and internal IPs are classified as `sensitive`
- [ ] Restricted memories are deleted after 7 days by maintenance
- [ ] Sensitive memories are deleted after 90 days by maintenance
- [ ] Ephemeral memories cannot be promoted beyond L0
- [ ] Cross-workspace retrieval excludes sensitive and restricted memories
- [ ] All scan actions are recorded in provenance
- [ ] Content scanning adds <5ms to insertion latency

## Owner and Effort

- **Owner:** TBD
- **Effort:** M
- **Dependencies:** None. Complements memory write quality gates proposal (quality gates focus on relevance/stability; safety gates focus on sensitivity/retention).

## Rollout and Rollback

- **Rollout:** Migration adds `sensitivity` column with default `normal`. Existing memories are unaffected. Scanning applies only to new insertions.
- **Rollback:** Revert commit. `sensitivity` column remains with default values. No behavioral change.
- **Blast radius:** New insertions may be blocked or redacted. Provenance provides full audit trail for debugging false positives.

## Test Plan

- [ ] Unit test: private key content is blocked
- [ ] Unit test: `password=hunter2` is redacted to `password=[REDACTED]`
- [ ] Unit test: email address classified as `sensitive`
- [ ] Unit test: internal IP classified as `sensitive`
- [ ] Unit test: restricted memory deleted after 7-day retention
- [ ] Unit test: sensitive memory deleted after 90-day retention
- [ ] Unit test: ephemeral insert sets correct tier/sensitivity/valid_until
- [ ] Unit test: cross-workspace query excludes sensitive memories
- [ ] Integration test: scan + classify + retention lifecycle end-to-end
- [ ] Manual: insert test content with known patterns, verify classification

## Operational Impact

- **Metrics:** Scan block/redact/classify counts in `aimee memory stats`.
- **Logging:** Blocked content logged at warn (without the sensitive content itself). Redactions at info. Classifications at debug.
- **Alerts:** None.
- **Disk/CPU/Memory:** Regex scanning per insertion. Retention enforcement is a single DELETE per class per maintenance cycle. Negligible.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Content scanning (block + redact) | P1 | S | Security baseline |
| Sensitivity classification | P1 | S | Enables retention policies |
| Retention enforcement | P1 | S | Prevents indefinite sensitive data retention |
| Ephemeral insert | P2 | S | Nice-to-have for explicit "don't memorize" |
| Retrieval filtering | P2 | S | Cross-workspace hygiene |

## Trade-offs

**Why regex instead of a machine learning classifier?** Secrets have well-defined syntactic patterns (key prefixes, base64 blocks, `key=value` formats). Regex catches >90% of cases with zero dependencies. A classifier would add latency and a model dependency for marginal improvement on the long tail.

**Why separate from the quality gates proposal?** Quality gates decide "is this worth storing?" Safety gates decide "is this safe to store?" They operate on different criteria and have different failure modes. A quality gate rejection means "try again with better content." A safety gate rejection means "this must never be persisted." Keeping them separate makes the failure semantics clear.

**Why not encrypt sensitive memories at rest?** SQLite doesn't support column-level encryption natively. Encrypting the content field would require application-level encrypt/decrypt on every read and write, breaking FTS5 indexing. For a single-user system on local disk, file-system-level encryption (LUKS, FileVault) is more appropriate. If multi-user or remote storage becomes relevant, this can be revisited.
