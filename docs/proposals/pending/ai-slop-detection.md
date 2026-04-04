# Proposal: AI Slop Detection and Cleanup

## Problem

Aimee has multiple overlapping ideas for catching low-quality AI-generated code, but the work is split across separate proposals for comment linting, broad slop detection, and explicit cleanup commands. That fragmentation makes priority unclear and encourages half-solutions:

- warning-only comment checks miss broader structural issues
- automatic post-write detection without a cleanup path leaves the agent to ignore findings
- a standalone cleanup command without shared detection logic duplicates heuristics

The actual need is one coherent flow: cheap detection on writes, actionable findings in-session, and an explicit cleanup path when the user wants it.

## Goals

- Detect common AI-slop patterns immediately after file writes.
- Cover both comment-level slop and structural slop.
- Keep detection cheap and advisory by default.
- Provide an explicit `aimee clean` workflow for targeted cleanup and optional delegate-assisted fixes.
- Make rules conservative enough that agents do not learn to ignore them.

## Approach

Build one shared slop-detection subsystem and expose it in two modes:

1. `post_tool_update()` runs lightweight detection after `Write`/`Edit`.
2. `aimee clean` runs the same detector on demand, with optional `--fix` delegation.

### Detection Scope

Use a fast C implementation with conservative heuristics:

- verbose or redundant comments
- placeholder TODOs and AI-attribution comments
- dead code and unreachable branches
- pass-through wrappers and needless abstraction
- duplicate blocks within a file
- speculative error handling or over-defensive null checks

Keep/remove guidance should be explicit so `--fix` has bounded behavior:

- keep comments that explain business rules, algorithms, regexes, or issue history
- remove comments that restate code or leave placeholders behind
- keep validation at boundaries and real I/O failure handling
- remove obviously redundant defensive checks and pass-through boilerplate

### User Flows

```bash
aimee clean src/foo.c
aimee clean --changed
aimee clean --fix src/foo.c
```

- default mode reports findings with `file:line`
- `--fix` delegates a focused cleanup using the detector output as guardrails
- after `--fix`, rerun detection and existing verification to confirm improvement

### Changes

| File | Change |
|------|--------|
| `src/slop_detect.c` | New shared detector for comment and structural slop |
| `src/headers/slop_detect.h` | Finding types, severity, and detector API |
| `src/guardrails.c` | Run detection after file writes and append findings to tool output/session context |
| `src/cmd_core.c` | Add `clean` subcommand and `--fix` mode |
| `src/git_verify.c` | Optional verification step for changed-file slop checks |
| `src/tests/test_slop_detect.c` | Detector tests and false-positive guards |

## Acceptance Criteria

- [ ] One detector covers comment slop and structural slop categories.
- [ ] Post-write checks append advisory findings without blocking edits.
- [ ] `aimee clean <file>` reports findings with stable file:line output.
- [ ] `aimee clean --fix <file>` delegates cleanup and reruns detection.
- [ ] Legitimate comments and boundary validation are preserved.
- [ ] Detection stays fast enough for post-write use, target `<50ms` on typical files.

## Owner and Effort

- **Owner:** aimee
- **Effort:** M (3-4 focused sessions)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Ship in phases: detector first, post-write integration second, `--fix` last.
- **Rollback:** Revert detector integration or disable the verification step; `aimee clean` remains additive.
- **Blast radius:** Advisory by default. Worst case is noisy findings or low-value cleanup diffs.

## Test Plan

- [ ] Unit tests for each slop category with known-good and known-bad fixtures.
- [ ] Unit tests covering false positives on legitimate comments and validation code.
- [ ] Integration test: write a file with slop and confirm findings appear in tool output.
- [ ] Integration test: `aimee clean --fix` improves a file without changing behavior.
- [ ] Performance test on 1k+ line files.

## Operational Impact

- **Metrics:** `slop_findings_total{category}`, `slop_detect_latency_ms`, `slop_fix_runs_total`
- **Logging:** INFO summary for explicit clean runs, DEBUG for automatic post-write findings
- **Alerts:** None
- **Disk/CPU/Memory:** Negligible outside explicit `--fix` delegate runs

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Shared detector | P1 | M | High |
| Post-write advisory integration | P1 | S | High |
| `aimee clean` reporting | P2 | S | Medium |
| `--fix` delegate cleanup | P3 | S | Medium |

## Trade-offs

- **Why merge detection and cleanup?** The cleanup path is only safe if it reuses the exact same detector and keep/remove rules.
- **Why stay advisory?** Blocking on heuristic code-quality checks will quickly become counterproductive.
- **Why keep `aimee clean` explicit?** Automatic mutation during normal edits is too aggressive; explicit cleanup keeps the risk bounded.
