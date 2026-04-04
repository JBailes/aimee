# Proposal: Kind-Specific Lifecycle Rules & New Memory Kinds

## Problem

All memory kinds share the same promotion, demotion, and expiry thresholds (`PROMOTE_L1_USE_COUNT 3`, `DEMOTE_L2_DAYS 60`, `EXPIRE_L1_DAYS 30`). This is wrong for several reasons:

1. **Policies used once a quarter shouldn't be demoted.** A compliance rule ("never store session tokens in cookies") may only be accessed when the agent touches auth code. 60 days of disuse doesn't mean it's stale — it means the agent hasn't done auth work recently.

2. **Procedures are high-value, low-frequency.** A troubleshooting runbook for "PostgreSQL cert auth failure" might be accessed twice a year but is critical each time. The current system would demote it from L2 after 60 days of disuse.

3. **Scratches that survive a session are misclassified.** If an L0 scratch is worth keeping beyond a session, it should be reclassified as a fact or decision, not promoted as-is.

4. **The existing 6 kinds don't capture procedural knowledge or policy constraints.** These are distinct categories with different trust, TTL, and retrieval characteristics.

Evidence: `memory_promote.c` applies identical thresholds regardless of kind. The constants in `aimee.h:42-48` are global. A coding convention and a temporary task get the same lifecycle treatment.

## Goals

- Different memory kinds have different lifecycle rules (TTL, promotion thresholds, demotion resistance).
- Two new kinds (`procedure`, `policy`) capture knowledge that the current 6 kinds miss.
- Low-frequency, high-value memories (procedures, policies, conventions) resist demotion.
- The lifecycle engine is data-driven, not hardcoded per kind.

## Approach

### 1. New Memory Kinds

Add two new kinds to `aimee.h`:

```c
#define KIND_PROCEDURE  "procedure"  /* reusable workflows, runbooks, troubleshooting steps */
#define KIND_POLICY     "policy"     /* constraints, guardrails, compliance rules, org conventions */
```

**Procedure** examples:
- "To debug cert auth: check CA chain, verify SPIRE SVID expiry, test with openssl s_client"
- "Deploy sequence: run tests, merge, wait for CI, tag release, push"

**Policy** examples:
- "Never store session tokens in cookies (legal/compliance)"
- "Always check PR merge state before pushing"
- "Integration tests must hit real database, not mocks"

### 2. Kind Lifecycle Configuration Table

Replace hardcoded constants with a `kind_lifecycle` table populated by migration:

```sql
CREATE TABLE kind_lifecycle (
    kind TEXT PRIMARY KEY,
    promote_use_count INTEGER NOT NULL DEFAULT 3,
    promote_confidence REAL NOT NULL DEFAULT 0.9,
    demote_days INTEGER NOT NULL DEFAULT 60,
    demote_confidence REAL NOT NULL DEFAULT 0.7,
    expire_days INTEGER NOT NULL DEFAULT 30,
    demotion_resistance REAL NOT NULL DEFAULT 1.0  /* multiplier on demote_days */
);

INSERT INTO kind_lifecycle VALUES
    ('fact',       3,  0.9,  60,  0.7,  30,  1.0),
    ('preference', 2,  0.8,  90,  0.6,  30,  1.5),
    ('decision',   3,  0.9,  90,  0.7,  45,  1.5),
    ('episode',    5,  0.9,  30,  0.7,  14,  0.5),
    ('task',       3,  0.9,  14,  0.7,   7,  0.5),
    ('scratch',    5,  0.95, 7,   0.7,   3,  0.25),
    ('procedure',  2,  0.8,  180, 0.5,  90,  3.0),
    ('policy',     1,  0.7,  365, 0.3,  180, 5.0);
```

Key design choices:
- **Policies** promote easily (1 use, 0.7 confidence) because they're typically authoritative from the start. They resist demotion aggressively (5x multiplier = effectively 1825 days before demotion check).
- **Procedures** promote at 2 uses and resist demotion at 3x (540 effective days).
- **Episodes** and **tasks** are ephemeral — they expire faster and demote easier.
- **Scratches** are aggressively expired (3 days at L1).

### 3. Lifecycle Engine Changes

Modify `memory_promote.c` to query `kind_lifecycle` instead of using constants:

```c
/* Load lifecycle config for a kind */
typedef struct {
    int promote_use_count;
    double promote_confidence;
    int demote_days;
    double demote_confidence;
    int expire_days;
    double demotion_resistance;
} kind_lifecycle_t;

int kind_lifecycle_load(sqlite3 *db, const char *kind, kind_lifecycle_t *out);
```

The promote, demote, and expire functions use the loaded config instead of `#define` constants. The effective demote threshold becomes:

```c
int effective_demote_days = (int)(lifecycle.demote_days * lifecycle.demotion_resistance);
```

### 4. Kind Inference at Write Time

When a memory is inserted without an explicit kind, apply heuristic classification:

- Content contains "always", "never", "must", "must not", "do not" -> suggest `policy`
- Content contains "steps:", "sequence:", "to debug", "to deploy", "runbook" -> suggest `procedure`
- Otherwise: use the caller-specified kind (no change from current behavior)

This is a soft suggestion logged at info level, not an override. The caller retains control.

### Changes

| File | Change |
|------|--------|
| `src/db.c` | New migration: `kind_lifecycle` table with default rows |
| `src/headers/aimee.h` | Add `KIND_PROCEDURE`, `KIND_POLICY`, `kind_lifecycle_t` struct |
| `src/memory_promote.c` | Load lifecycle config per kind, replace hardcoded constants |
| `src/memory.c` | Validate new kinds in `memory_insert()`, optional kind inference |
| `src/cmd_hooks.c` | Include procedures/policies in context assembly sections |
| `src/memory_context.c` | New context section for procedures/policies (or merge into existing) |

## Acceptance Criteria

- [ ] `procedure` and `policy` kinds can be inserted, queried, and displayed
- [ ] Each kind uses its own lifecycle thresholds from `kind_lifecycle` table
- [ ] Policy memories resist demotion for at least 365 * 5 = 1825 effective days
- [ ] Procedure memories resist demotion for at least 180 * 3 = 540 effective days
- [ ] Episode and task memories expire faster than facts (14 and 7 days respectively)
- [ ] `kind_lifecycle` table is editable by the operator (no recompile to tune)
- [ ] Existing memories continue to work with their current kinds
- [ ] Kind inference logs suggestions but does not override explicit kinds

## Owner and Effort

- **Owner:** TBD
- **Effort:** M
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Migration creates `kind_lifecycle` table with defaults. Existing memories unaffected — their kinds map to the same thresholds they had before (fact, preference, decision, episode, task, scratch all get their current values).
- **Rollback:** Revert commit. `kind_lifecycle` table remains but is unused. Lifecycle code falls back to hardcoded constants.
- **Blast radius:** Changes promotion/demotion timing for existing kinds. The default values match current behavior, so no change unless the operator edits the table.

## Test Plan

- [ ] Unit test: `kind_lifecycle_load()` returns correct defaults for all 8 kinds
- [ ] Unit test: policy memory with 1 use promotes to L2
- [ ] Unit test: policy memory at L2 is not demoted after 365 days of disuse
- [ ] Unit test: scratch memory at L1 expires after 3 days
- [ ] Unit test: procedure memory promotes at 2 uses
- [ ] Unit test: unknown kind falls back to `fact` lifecycle
- [ ] Integration test: end-to-end insert procedure, promote, verify demotion resistance
- [ ] Manual: edit `kind_lifecycle` row, verify changed behavior

## Operational Impact

- **Metrics:** Per-kind promotion/demotion/expiry counts in `aimee memory stats`.
- **Logging:** Lifecycle config loaded at startup, logged at debug.
- **Alerts:** None.
- **Disk/CPU/Memory:** One additional query per maintenance cycle per kind (8 queries). Negligible.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Kind lifecycle table | P1 | M | Foundation for correct lifecycle behavior |
| Procedure + policy kinds | P2 | S | Fills representation gap |
| Kind inference | P3 | S | Nice-to-have, not critical |

## Trade-offs

**Why a database table instead of config file?** The lifecycle table is operational data, not configuration. It changes per-deployment and should be queryable/updatable without restarting. A database table is the right home for tunable thresholds that the operator may want to adjust based on observed behavior.

**Why `demotion_resistance` as a multiplier?** A multiplier composes cleanly with the base `demote_days`. Setting resistance to 5.0 means "this kind is 5x harder to demote than baseline." This is more intuitive than a separate absolute threshold per kind and scales naturally if the baseline changes.

**Why not more kinds?** The 8-kind taxonomy (fact, preference, decision, episode, task, scratch, procedure, policy) covers the knowledge types that appear in engineering work. Adding more kinds increases cognitive overhead for both the operator and the lifecycle engine. If a new category emerges, a row in `kind_lifecycle` is all that's needed — no code change.
