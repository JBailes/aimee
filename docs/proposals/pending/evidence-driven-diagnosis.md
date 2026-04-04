# Proposal: Evidence-Driven Diagnosis Mode (`aimee diagnose`)

## Problem

When investigating bugs, performance issues, or infrastructure problems, the primary agent typically jumps to a hypothesis and starts fixing. This wastes tokens when the initial hypothesis is wrong, and it risks fixing symptoms rather than root causes.

Aimee's trace analysis (`trace_analysis.c`) detects patterns in *agent execution* (retry loops, common tool sequences), but there is no structured process for investigating *system behavior* — correlating logs, metrics, code paths, and observed symptoms into a root cause analysis.

oh-my-codex's `$analyze` skill enforces a structured investigation methodology: preserve the distinction between observation/hypothesis/evidence, rank evidence hierarchically, actively falsify leading hypotheses, and surface critical unknowns rather than collapsing into "fake certainty."

Evidence:
- `trace_analysis.c` mines agent execution traces, not system diagnostic data
- No structured diagnosis workflow exists
- Delegates can be asked to investigate but there's no framework for correlating their findings
- The primary agent currently conflates observation with hypothesis

## Goals

- A structured diagnosis mode that separates observations, hypotheses, and evidence
- Multiple investigation threads can run in parallel (one per hypothesis)
- Evidence is ranked: direct experiment > log correlation > code reading > speculation
- The diagnosis produces a ranked hypothesis list with supporting/contradicting evidence
- A "discriminating probe" recommendation identifies the single highest-value next step

## Approach

### 1. Diagnosis session model

```sql
CREATE TABLE IF NOT EXISTS diagnoses (
    id INTEGER PRIMARY KEY,
    symptom TEXT NOT NULL,
    status TEXT DEFAULT 'active',  -- active, concluded, abandoned
    conclusion TEXT,
    confidence REAL,
    created_at TEXT DEFAULT (datetime('now')),
    updated_at TEXT DEFAULT (datetime('now'))
);

CREATE TABLE IF NOT EXISTS diagnosis_items (
    id INTEGER PRIMARY KEY,
    diagnosis_id INTEGER NOT NULL REFERENCES diagnoses(id),
    kind TEXT NOT NULL,  -- observation, hypothesis, evidence_for, evidence_against, unknown, probe
    parent_id INTEGER,  -- hypothesis ID this evidence relates to (NULL for observations)
    content TEXT NOT NULL,
    source TEXT,  -- file:line, log path, command output, delegate name
    evidence_rank INTEGER DEFAULT 3,  -- 1=direct experiment, 2=log/metric, 3=code reading, 4=speculation
    created_at TEXT DEFAULT (datetime('now'))
);
```

### 2. Diagnosis workflow

```c
int diagnose_start(app_ctx_t *ctx, const char *symptom);
int diagnose_add_observation(app_ctx_t *ctx, int diag_id, const char *observation, const char *source);
int diagnose_add_hypothesis(app_ctx_t *ctx, int diag_id, const char *hypothesis);
int diagnose_investigate(app_ctx_t *ctx, int diag_id, int hypothesis_id);
int diagnose_conclude(app_ctx_t *ctx, int diag_id);
```

`diagnose_investigate` delegates investigation of a specific hypothesis:
1. Gather the hypothesis and all observations
2. Delegate to `reason` role: "Investigate this hypothesis. Gather evidence for AND against. Suggest the single best discriminating probe if inconclusive."
3. Parse the response into evidence_for, evidence_against, and probe items
4. Store in diagnosis_items

### 3. Parallel hypothesis investigation

Multiple hypotheses can be investigated in parallel via delegates. The coordination:
1. Generate initial hypotheses (delegate `reason` with symptoms + observations)
2. For each hypothesis, dispatch an investigation delegate
3. Collect results, rank hypotheses by evidence balance
4. If no clear winner, execute the recommended discriminating probe
5. Re-evaluate with new evidence

### 4. Output format

```
# Diagnosis: service returns 502 after deploy

## Observations
1. [log] 502 errors started at 14:23 UTC (deploy completed 14:21)
2. [metric] CPU spike to 95% on wol-web at 14:22
3. [code] New endpoint handler doesn't close DB connections

## Hypotheses (ranked by evidence)
### H1: Connection pool exhaustion (confidence: 0.8)
  + [direct] Connection count rises linearly after deploy (pg_stat_activity)
  + [code] New handler opens connection but missing defer close
  - [log] No "connection refused" errors (would expect them if pool exhausted)
  → Probe: Check pg_stat_activity count over 10 minutes

### H2: OOM kill (confidence: 0.3)
  + [metric] Memory trending up
  - [direct] No OOM entries in dmesg
  - [metric] Memory stabilized at 80%, never reached limit

## Recommended next step
Run `SELECT count(*) FROM pg_stat_activity` every 60s for 10 minutes to confirm connection leak.
```

### 5. CLI and MCP

```bash
aimee diagnose "502 errors after deploy"           # start diagnosis
aimee diagnose observe <id> "CPU spike at 14:22"   # add observation
aimee diagnose investigate <id>                     # run parallel hypothesis investigation
aimee diagnose status <id>                          # show current hypothesis ranking
aimee diagnose conclude <id>                        # finalize with conclusion
```

MCP:
```json
{
  "name": "diagnose_start",
  "description": "Start structured evidence-driven diagnosis of a problem",
  "parameters": {
    "symptom": "string"
  }
}
```

### Changes

| File | Change |
|------|--------|
| `src/agent_diagnose.c` | New: diagnosis model, investigation delegation, hypothesis ranking |
| `src/headers/agent_diagnose.h` | New: diagnosis types and function declarations |
| `src/db.c` | Add `diagnoses` and `diagnosis_items` table migrations |
| `src/cmd_core.c` | Add `diagnose` subcommand |
| `src/mcp_tools.c` | Add `diagnose_start`, `diagnose_investigate` MCP tools |
| `src/tests/test_diagnose.c` | Tests for hypothesis ranking, evidence storage |

## Acceptance Criteria

- [ ] `aimee diagnose "symptom"` starts a structured diagnosis session
- [ ] Observations, hypotheses, and evidence are stored separately with proper relationships
- [ ] Evidence is ranked (direct experiment > log > code > speculation)
- [ ] Parallel hypothesis investigation via delegates works
- [ ] `aimee diagnose status` shows ranked hypotheses with evidence balance
- [ ] Discriminating probe recommendations are generated when evidence is inconclusive

## Owner and Effort

- **Owner:** aimee
- **Effort:** M (3-5 focused sessions)
- **Dependencies:** Delegation infrastructure (existing)

## Rollout and Rollback

- **Rollout:** New tables, new commands. No changes to existing behavior.
- **Rollback:** Revert commit. Drop tables.
- **Blast radius:** None — entirely additive.

## Test Plan

- [ ] Unit tests: hypothesis ranking with various evidence combinations
- [ ] Unit tests: evidence rank ordering
- [ ] Unit tests: parallel investigation dispatch
- [ ] Integration tests: end-to-end symptom → investigation → ranked hypotheses
- [ ] Manual verification: diagnose a known issue, verify hypothesis ranking matches reality

## Operational Impact

- **Metrics:** `diagnoses_started`, `diagnoses_concluded`, `diagnosis_hypotheses_investigated`
- **Logging:** Investigation progress: `aimee: diagnose #N: investigating 3 hypotheses in parallel`
- **Alerts:** None
- **Disk/CPU/Memory:** 1 delegate call per hypothesis investigation. Diagnosis items ~200 bytes each.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Diagnosis model + evidence storage | P1 | M | High — structured investigation |
| Parallel hypothesis investigation | P1 | S | High — efficient root cause analysis |
| Evidence ranking + probe recommendations | P1 | S | High — actionable output |
| CLI interface | P2 | S | Medium |
| MCP tools | P2 | S | Medium |

## Trade-offs

**Why not extend trace_analysis.c?**
Trace analysis mines agent execution patterns automatically. Diagnosis is an interactive investigation process driven by the user/agent with different inputs (symptoms, logs, metrics) and different outputs (ranked hypotheses). They serve different purposes.

**Why store diagnosis items in a relational table instead of JSON?**
Items need to be queried (all evidence for hypothesis H1, all observations, all probes). A relational model with parent_id references supports these queries naturally. JSON blobs would require client-side filtering.

**Why delegate investigation instead of doing it in C?**
Investigation requires reasoning about symptoms, code, and system behavior. This is fundamentally an LLM task. The C code provides the structured framework; delegates provide the reasoning.
