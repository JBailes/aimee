# Proposal: Structured Multi-Perspective Code Review

## Problem

Aimee's `verify` command runs delegate-based review on current changes, but the review is single-perspective and unstructured. The delegate gets a diff and returns freeform text. There is no:
- Categorical analysis (security, performance, quality, maintainability)
- Severity classification (critical vs. low)
- Parallel multi-perspective review (security reviewer + code reviewer + architect)
- Structured output format that can be programmatically processed

This means review quality depends entirely on the delegate's prompt interpretation, and the primary agent has no structured data to act on — just prose to parse.

oh-my-codex implements multi-stage categorical review: a five-domain analysis (security, quality, performance, best practices, maintainability) with severity stratification and parallel multi-reviewer passes. Their security-review skill adds OWASP-structured vulnerability scanning. The result is structured, actionable, and composable.

Evidence:
- `git_verify.c` runs build/test/lint steps but has no code review logic
- `aimee verify` delegates to a reviewer but the prompt is generic
- No structured review output format exists
- Security review is not a separate concern — it's mixed into general review

## Goals

- Code review produces structured findings with severity, category, file:line, and suggested fix
- Multiple review perspectives run in parallel (security, quality, architecture)
- Findings are stored in the DB, not just printed — enabling tracking and regression detection
- Critical findings block the verify gate; low findings are advisory
- The review framework is extensible — new perspectives can be added without code changes

## Approach

### 1. Review findings model

```sql
CREATE TABLE IF NOT EXISTS review_findings (
    id INTEGER PRIMARY KEY,
    session_id TEXT NOT NULL,
    plan_id INTEGER,
    category TEXT NOT NULL,  -- security, quality, performance, maintainability, best_practice
    severity TEXT NOT NULL,  -- critical, high, medium, low
    file_path TEXT,
    line_number INTEGER,
    description TEXT NOT NULL,
    suggestion TEXT,
    reviewer TEXT,  -- delegate name that produced this finding
    status TEXT DEFAULT 'open',  -- open, fixed, dismissed
    created_at TEXT DEFAULT (datetime('now'))
);
```

### 2. Review perspectives

Define review perspectives as JSON configurations, not hardcoded:

```json
[
  {
    "name": "security",
    "role": "review",
    "prompt_template": "Review this diff for security vulnerabilities. Check: injection (SQL, command, XSS), authentication/authorization issues, credential exposure, insecure cryptography, SSRF. For each finding, provide: category=security, severity, file:line, description, suggestion.",
    "blocking_severities": ["critical", "high"]
  },
  {
    "name": "quality",
    "role": "review",
    "prompt_template": "Review this diff for code quality. Check: duplication, dead code, unnecessary abstraction, unclear naming, missing error handling at system boundaries. For each finding, provide: category=quality, severity, file:line, description, suggestion.",
    "blocking_severities": ["critical"]
  },
  {
    "name": "architecture",
    "role": "reason",
    "prompt_template": "Review this diff for architectural soundness. Check: boundary violations, coupling, layering, API contract changes, backwards compatibility. For each finding, provide: category=maintainability, severity, file:line, description, suggestion.",
    "blocking_severities": ["critical"]
  }
]
```

Store perspectives in `~/.config/aimee/review_perspectives.json`. Ship a default set; users can customize.

### 3. Parallel multi-perspective review

Extend `aimee verify` to run all configured perspectives in parallel:

```c
int verify_structured_review(app_ctx_t *ctx, const char *diff, int plan_id);
```

1. Load all perspectives from config
2. For each perspective, delegate the review with the diff and the perspective's prompt template
3. Parse structured findings from each delegate response (JSON array)
4. Insert findings into `review_findings`
5. Check if any blocking findings exist
6. Return pass/fail with summary

### 4. Structured output parsing

Delegate prompts request JSON output:

```json
[
  {
    "category": "security",
    "severity": "high",
    "file": "src/server.c",
    "line": 42,
    "description": "Command string built from user input without sanitization",
    "suggestion": "Use parameterized execution or validate/escape the input"
  }
]
```

If the delegate returns non-JSON (graceful degradation), wrap the text as a single finding with severity=medium and category matching the perspective name.

### 5. CLI integration

```bash
aimee verify                     # existing: build+test+lint + NEW structured review
aimee verify --review-only       # skip build/test, run only structured review
aimee verify findings            # list open findings for current session
aimee verify findings --severity critical,high  # filter
aimee verify dismiss <id>        # mark a finding as dismissed with reason
```

### 6. MCP tool

```json
{
  "name": "structured_review",
  "description": "Run multi-perspective structured code review on current changes",
  "parameters": {
    "diff": "string (optional, auto-detected from git if omitted)",
    "perspectives": "array of strings (optional, defaults to all configured)"
  }
}
```

### Changes

| File | Change |
|------|--------|
| `src/git_verify.c` | Add `verify_structured_review()` with parallel delegate dispatch and finding parsing |
| `src/db.c` | Add `review_findings` table migration |
| `src/mcp_tools.c` | Add `structured_review` MCP tool |
| `src/cmd_core.c` | Extend `verify` subcommand with `--review-only`, `findings`, `dismiss` |
| `src/tests/test_structured_review.c` | Tests for finding parsing, severity gating, parallel dispatch |

## Acceptance Criteria

- [ ] `aimee verify` runs configured review perspectives in parallel alongside build/test/lint
- [ ] Findings are stored in `review_findings` with category, severity, file:line
- [ ] Critical/high security findings block the verify gate
- [ ] `aimee verify findings` lists open findings
- [ ] Perspectives are configurable via `review_perspectives.json`
- [ ] Non-JSON delegate responses degrade gracefully to unstructured findings
- [ ] `structured_review` MCP tool is callable

## Owner and Effort

- **Owner:** aimee
- **Effort:** M (3-4 focused sessions)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** New table, new config file with defaults. Existing `aimee verify` gains structured review as an additive step.
- **Rollback:** Revert commit. Drop table. Verify reverts to unstructured behavior.
- **Blast radius:** Low. Additive to existing verify. Worst case: verify takes slightly longer due to parallel review delegates.

## Test Plan

- [ ] Unit tests: finding JSON parsing — valid, malformed, empty
- [ ] Unit tests: severity gating — critical blocks, low doesn't
- [ ] Unit tests: graceful degradation for non-JSON responses
- [ ] Integration tests: end-to-end diff → parallel review → findings stored → gate verdict
- [ ] Manual verification: introduce a known vulnerability, observe it flagged as critical

## Operational Impact

- **Metrics:** `review_findings_total{category,severity}`, `review_perspectives_run`, `review_gate_blocks`
- **Logging:** Per-perspective: `aimee: verify review [security]: 2 findings (1 high, 1 low)`
- **Alerts:** None
- **Disk/CPU/Memory:** N delegate calls in parallel (one per perspective). Findings ~200 bytes each.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Multi-perspective parallel review | P1 | M | High — core value |
| Structured finding storage | P1 | S | High — enables tracking |
| Severity-based gating | P1 | S | High — makes review actionable |
| Configurable perspectives | P2 | S | Medium — extensibility |
| CLI findings management | P2 | S | Medium — user interface |

## Trade-offs

**Why parallel instead of sequential reviews?**
Unlike consensus planning (where critic needs architect's output), review perspectives are independent. A security reviewer doesn't need quality review results. Parallel saves latency.

**Why store findings in DB instead of just printing them?**
Stored findings enable: tracking which findings were fixed vs. dismissed, detecting regressions (same finding reappearing), and feeding findings into the completion loop for automatic fix attempts.

**Why configurable perspectives instead of hardcoded?**
Different projects have different review priorities. A C project needs memory safety review; a web app needs XSS review. Users should be able to add domain-specific perspectives.
