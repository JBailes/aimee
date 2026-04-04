# Proposal: Project-Scoped Workflow Learning

## Problem

Aimee has no mechanism to learn and surface project-specific operational rules from interactions. When a user corrects an agent ("SmoothNAS PRs target `testing`, not `main`"), that knowledge is lost unless the user manually stores a memory. Even if stored, project-scoped memories are poorly surfaced: the session context query (`cmd_hooks.c:334`) uses fragile LIKE matching on project name in key/content, limited to 3 items at 150 chars.

Evidence:
- In a live session, an agent created a PR targeting `main` for SmoothNAS, which requires PRs to target `testing` first. There was no way for aimee to know this.
- The `memory_workspaces` join table exists but has exactly 1 row in production (`_shared`). Workspace tagging is effectively dead infrastructure.
- The session context project section uses `WHERE key LIKE '%SmoothNAS%' OR content LIKE '%SmoothNAS%'` instead of joining on `memory_workspaces`. This means a fact must mention the project name verbatim to be surfaced.
- Project context is capped at 3 memories × 150 chars = 450 chars total. Workflow rules need more space.

## Goals

- Project-specific workflow rules (branch strategy, deploy procedures, test commands) are automatically learned from observed agent actions and user corrections.
- Workflow memories are stored in the existing DB with proper workspace tagging — no new files or parallel storage.
- When working in a project directory, relevant workflow rules are surfaced in the session context with high priority.
- The `memory_workspaces` infrastructure is actually used for context assembly, replacing LIKE matching.

## Approach

### 1. Add `workflow` memory kind

No schema changes needed — `kind` is freetext. Add a new kind value `workflow` for operational rules that describe *how work is done* in a project:

- Branch/PR strategy ("PRs target `testing`, then `testing` merges to `main`")
- Build/test commands ("run `make test` before PRs")
- Deploy procedures ("deploy via rsync to `10.0.0.x`")
- Review requirements ("needs 1 approval before merge")

Workflow memories are distinct from `fact` (which stores declarative knowledge about infrastructure/architecture) and `procedure` (which stores step-by-step recipes). A workflow rule is a constraint or convention that should be enforced automatically.

Define the constant in `headers/memory_types.h`:

```c
#define KIND_WORKFLOW "workflow"
```

### 2. Fix project context surfacing — use workspace tags

Replace the LIKE-based project context query in `build_session_context()` (`cmd_hooks.c:334`) with a proper JOIN on `memory_workspaces`:

```sql
SELECT m.key, m.content, m.kind FROM memories m
  JOIN memory_workspaces mw ON mw.memory_id = m.id
  WHERE mw.workspace = ?
    AND m.tier IN ('L1', 'L2', 'L3')
  ORDER BY
    CASE m.kind WHEN 'workflow' THEN 0 WHEN 'decision' THEN 1 ELSE 2 END,
    CASE m.tier WHEN 'L3' THEN 0 WHEN 'L2' THEN 1 ELSE 2 END,
    m.use_count DESC
  LIMIT 10
```

Key changes:
- **JOIN on `memory_workspaces`** instead of LIKE matching — precise, no false positives
- **Workflow kind prioritized** — workflow rules appear before facts/episodes
- **Limit raised to 10** — from 3, giving room for workflow rules alongside facts
- **Truncation raised to 300 chars** — from 150, workflow rules need more space
- **Section header includes kind** — e.g., "workflow: PRs target testing" vs "fact: uses Go backend"

Also emit the `_shared` workspace memories in a separate "# Shared Context" section, so cross-cutting rules (network topology, deploy conventions) are always visible regardless of which project directory the agent is in.

### 3. PostToolUse workflow learning

Extend `post_tool_update()` in `guardrails.c` to observe workflow signals from tool usage and auto-store workflow memories. The function already fires on every PostToolUse hook call.

**Signals to observe:**

| Tool | Signal | Learned rule |
|------|--------|-------------|
| Bash (`gh pr create --base X`) | PR created targeting non-default branch | "PRs target branch X" |
| Bash (`git push origin X`) | Push to non-main branch | "Active development on branch X" |
| Bash (`make test`, `npm test`, etc.) | Test command executed | "Test command: `make test`" |
| Bash (`rsync`, `scp` to known host) | Deploy command | "Deploy via rsync to host Y" |
| Bash (`gh pr merge`) | PR merged from specific branch | "Merge flow: X → Y" |

Implementation approach:
- Parse `tool_input` JSON for the command string (same as existing `is_git_command` check)
- Match against workflow signal patterns
- Determine the workspace from cwd (reuse `memory_auto_tag_workspace` logic)
- Upsert a workflow memory: if a matching key already exists for this workspace, update confidence; if new, insert at L1 with confidence 0.6
- Confidence increases with repeated observation (0.6 → 0.8 → 1.0), promoting naturally through the tier system

**Deduplication:** Use a composite key format: `workflow:{project}:{signal_type}` (e.g., `workflow:SmoothNAS:pr-target`). Upsert on key match — update content and bump confidence rather than creating duplicates.

### 4. User correction learning

When the user corrects a workflow (e.g., "no, PRs should target testing"), the agent can store this via `aimee memory store`. But we also want the PreToolUse hook to detect corrections in the conversation context.

Extend the `aimee memory store` command to:
- Auto-detect `workflow` kind when the content matches workflow patterns (mentions "branch", "PR", "deploy", "merge", "target")
- Auto-tag the workspace from cwd
- Set confidence to 1.0 for user-explicit stores (user corrections are high-confidence)

Additionally, add an MCP tool `store_workflow` that the primary agent can call directly:

```json
{
  "name": "store_workflow",
  "description": "Store a project workflow rule learned from the current interaction",
  "parameters": {
    "project": "string (auto-detected from cwd if omitted)",
    "rule": "string (the workflow rule)",
    "signal_type": "string (pr-target, deploy, test-command, merge-flow, convention)"
  }
}
```

### 5. Workflow memory lifecycle

Workflow memories follow the existing tier promotion lifecycle:

| Stage | Tier | Confidence | How it gets here |
|-------|------|-----------|-----------------|
| First observation | L1 | 0.6 | PostToolUse sees `gh pr create --base testing` |
| Repeated observation | L1 | 0.8+ | Same signal seen again, confidence bumped |
| Promotion | L2 | 0.8+ | `memory_promote` cycle promotes high-confidence L1 → L2 |
| User confirmation | L2 | 1.0 | User explicitly stores or confirms via correction |
| Permanent | L3 | 1.0 | Manual promotion or long-term stability |

Decay: if a workflow rule isn't observed for 90 days, confidence decays. If it contradicts a newer observation (e.g., PR target changed from `testing` to `develop`), the old rule is superseded.

### Changes

| File | Change |
|------|--------|
| `src/headers/memory_types.h` | Add `KIND_WORKFLOW "workflow"` constant |
| `src/cmd_hooks.c` | Rewrite project context query to JOIN on `memory_workspaces`; raise limit to 10, truncation to 300; prioritize workflow kind; add shared context section |
| `src/guardrails.c` | Extend `post_tool_update()` with workflow signal detection and auto-store |
| `src/memory.c` | Add `memory_upsert_workflow()` helper for deduped workflow storage |
| `src/mcp_server.c` | Add `store_workflow` MCP tool |
| `src/cmd_memory.c` | Auto-detect workflow kind in `mem_store` when content matches workflow patterns |
| `src/tests/test_guardrails.c` | Tests for PostToolUse workflow signal detection |
| `src/tests/test_workspace_memory.c` | Tests for workspace-tag-based context surfacing |

## Acceptance Criteria

- [ ] `aimee memory store --kind workflow --workspace SmoothNAS pr-target "PRs must target testing branch"` stores a workflow memory tagged to SmoothNAS
- [ ] Session context in a SmoothNAS worktree shows workflow rules under "# Project Context (SmoothNAS)" via workspace tag JOIN (not LIKE matching)
- [ ] `post_tool_update` detects `gh pr create --base testing` and auto-stores a workflow memory
- [ ] Repeated observation of the same workflow signal bumps confidence rather than creating duplicates
- [ ] Workflow memories appear before facts in project context
- [ ] `_shared` workspace memories appear in a "# Shared Context" section in all sessions
- [ ] `store_workflow` MCP tool is callable by the primary agent
- [ ] All existing unit tests continue to pass
- [ ] New tests cover: workflow signal detection, workspace-tag context assembly, deduplication

## Owner and Effort

- **Owner:** aimee
- **Effort:** M (3-5 focused sessions)
- **Dependencies:** None — builds on existing `memory_workspaces` table and `post_tool_update` hook

## Rollout and Rollback

- **Rollout:** Incremental. Ship the context surfacing fix (step 2) first — immediate improvement with no new data. Then add PostToolUse learning (step 3). MCP tool (step 4) last.
- **Rollback:** Revert commit. No schema migrations. Workflow memories are just rows with `kind='workflow'` — they can be deleted with `DELETE FROM memories WHERE kind='workflow'` if needed.
- **Blast radius:** Low. Context assembly changes affect all sessions but are additive (more context, not less). PostToolUse learning is append-only (stores memories, never deletes). Worst case: slightly longer context output.

## Test Plan

- [ ] Unit tests: `test_guardrails.c` — workflow signal detection for PR, deploy, test, merge patterns
- [ ] Unit tests: `test_workspace_memory.c` — workspace-tag-based context query returns correct memories, prioritizes workflow kind
- [ ] Unit tests: `memory_upsert_workflow` deduplication — same signal updates rather than duplicates
- [ ] Integration tests: end-to-end `gh pr create --base testing` → workflow memory stored → context surfaced in next session
- [ ] Failure injection: PostToolUse with malformed tool_input JSON — graceful no-op
- [ ] Manual verification: run a session in SmoothNAS, create a PR targeting testing, verify the workflow rule appears in the next session's context

## Operational Impact

- **Metrics:** New: `workflow_memories_stored` counter (auto-observed vs user-explicit). `project_context_memories_surfaced` counter.
- **Logging:** PostToolUse workflow detection logs to stderr: `aimee: learned workflow rule: {key} for {project}`.
- **Alerts:** None.
- **Disk/CPU/Memory:** Negligible. A few extra DB rows per session. The JOIN query is indexed (`idx_mw_workspace`).

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Fix context surfacing (step 2) | P1 | S | High — makes existing workspace tags useful |
| PostToolUse learning (step 3) | P1 | M | High — core learning mechanism |
| Workflow kind + lifecycle (steps 1, 5) | P2 | S | Medium — organizational clarity |
| MCP store_workflow tool (step 4) | P2 | S | Medium — explicit agent→memory path |
| User correction detection | P3 | S | Low — nice-to-have, manual store covers it |

## Trade-offs

**Why not a separate file per project (e.g., `SmoothNAS.workflow.md`)?**
Rejected. Files fragment state outside the DB, can't be queried/promoted/decayed, and create a parallel storage system. The DB already has the right schema — it just isn't being used.

**Why not a new tier (e.g., L2P for "project-scoped L2")?**
Rejected. Tiers represent confidence/permanence, not scope. Scope is orthogonal — handled by workspace tags. Adding project-specific tiers would double the tier system without adding semantic value.

**Why not use the LIKE query but improve it?**
The LIKE approach is fundamentally fragile. A memory about "SmoothNAS" must mention the string "SmoothNAS" in its key or content. A workspace tag is an explicit, indexed relationship that works regardless of content wording.

**Why start at L1/0.6 confidence for auto-observed workflows?**
Agent observations can be wrong — a one-time PR targeting `testing` might be an exception, not the rule. Starting at low confidence and requiring repeated observation prevents false rules from being surfaced. User-explicit stores bypass this with confidence 1.0.
