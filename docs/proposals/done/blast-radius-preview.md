# Proposal: Blast Radius Preview

## Problem

The guardrails system (src/guardrails.c) evaluates blast radius reactively: it checks files when they are about to be written or when a command is about to execute. There is no way for an agent to ask 'if I change files X, Y, Z, what would the blast radius be?' before starting work.

This means agents discover high-blast-radius situations mid-task, after they have already invested tool calls and context in an approach. The index.blast_radius server method exists but operates on a single file after the fact, not as a planning tool for a set of proposed changes.

## Goals

- Agents can preview the blast radius of a set of proposed file changes before starting work.
- Preview includes: affected files (direct dependents), severity classification, and warnings.
- Agents can use this to choose safer approaches or request approval before high-risk changes.

## Approach

### 1. New server method: blast_radius.preview

Accept a list of file paths and return the aggregate blast radius:

Input: { paths: [file1, file2, ...] }

Output: {
  total_dependents: N,
  severity: 'green|yellow|red',
  files: [
    { path: file1, dependents: [...], severity: 'green' },
    { path: file2, dependents: [...], severity: 'red' }
  ],
  warnings: ['file2 has 12 dependents, consider splitting the change']
}

### 2. Implementation

For each file in the input:
1. Look up the file in the code index (index_blast_radius in src/index.c)
2. Collect direct dependents (files that import/include/reference it)
3. Classify severity using existing thresholds from guardrails.c
4. Aggregate into a summary with the highest severity as the overall level

### 3. MCP tool exposure

Expose as an MCP tool preview_blast_radius so external agents (Claude Code, etc.) can use it during planning.

### 4. Warning generation

Generate actionable warnings based on patterns:
- Single file with >10 dependents: 'Consider splitting changes to reduce risk'
- Multiple red-severity files: 'This change set has high aggregate risk'
- Files in different subsystems: 'Changes span multiple subsystems, consider separate PRs'

### Changes

| File | Change |
|------|--------|
| src/server.c | Register blast_radius.preview dispatch method |
| src/server_compute.c | Implement blast_radius_preview() handler |
| src/index.c | Add index_blast_radius_multi() for batch lookups |
| src/mcp_server.c | Expose preview_blast_radius as MCP tool |

## Acceptance Criteria

- [ ] blast_radius.preview accepts a list of file paths and returns aggregate analysis
- [ ] Each file includes its dependents and severity classification
- [ ] Overall severity is the maximum of individual file severities
- [ ] Warnings are generated for high-risk patterns
- [ ] MCP tool is available for external agents
- [ ] Files not in the index return severity 'green' with empty dependents (safe default)

## Owner and Effort

- **Owner:** TBD
- **Effort:** S
- **Dependencies:** Code index must be populated (aimee index runs during session-start)

## Rollout and Rollback

- **Rollout:** Direct code change. New server method and MCP tool.
- **Rollback:** git revert. No state changes, purely additive.
- **Blast radius:** None. Read-only operation on existing index data.

## Test Plan

- [ ] Unit test: single file with known dependents returns correct count and severity
- [ ] Unit test: multiple files aggregate correctly (max severity wins)
- [ ] Unit test: unknown file returns green/empty
- [ ] Integration test: MCP tool returns valid JSON response
- [ ] Manual verification: preview a known high-impact file, verify warnings

## Operational Impact

- **Metrics:** None.
- **Logging:** None.
- **Alerts:** None.
- **Disk/CPU/Memory:** Read-only index lookups. O(n * d) where n is file count and d is average dependents. Negligible for typical change sets (<20 files).

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Blast radius preview | P2 | S | Prevents wasted work on high-risk approaches |

## Trade-offs

The preview is based on the code index, which may be stale if files were recently added or renamed. This is acceptable because the index is refreshed at session-start, and the preview is advisory (not a hard gate). Running a fresh index scan per preview request would be too slow (~seconds vs ~milliseconds for cached lookups).

Warning generation uses simple heuristics (threshold counts, subsystem detection by directory prefix). Sophisticated analysis (e.g., semantic change impact) would require parsing the actual diff, which is not available at preview time since the changes have not been made yet.
