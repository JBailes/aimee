# Proposal: Session Changelog

## Problem

When an agent starts a new session, it has no structured way to know what changed since its last session. It must manually run git log, check merged PRs, and diff branches. This wastes 3-5 tool calls per session start. The session-start hook (src/cmd_hooks.c:cmd_session_start) assembles state but includes no delta from the previous session. Agents working on multi-session tasks lose continuity.

## Goals

- Agents see a structured summary of changes since their last session at startup.
- No manual git commands needed to understand inter-session drift.
- Summary includes: merged PRs, new commits on main, branch movements, file-level diff stats.

## Approach

Add a changelog assembly step to cmd_session_start() that compares the current HEAD of tracked branches against the HEADs recorded in the previous session state. For each workspace, compute:

1. Commits on main since last session: git log --oneline <old_head>..HEAD
2. Merged PRs: parse merge commit subjects for PR numbers
3. Changed files: git diff --stat <old_head>..HEAD
4. Branch movements: compare worktree base refs

Store previous session HEADs in session_state_t. Emit the changelog as a structured JSON field in the launch metadata (returned by cmd_launch) and as a working memory entry for the session.

### Changes

| File | Change |
|------|--------|
| src/headers/guardrails.h | Add prev_main_head[64] to session_state_t |
| src/guardrails.c | Serialize/deserialize prev_main_head in state save/load |
| src/cmd_hooks.c | In cmd_session_start(): record current main HEAD, compute diff from prev_main_head, store changelog in working memory |
| src/cmd_hooks.c | In cmd_launch(): include changelog in launch metadata JSON |

## Acceptance Criteria

- [ ] Session state records the main branch HEAD at session start
- [ ] Next session computes and emits a changelog from the previous HEAD
- [ ] Changelog includes: commit count, file change stats, merged PR numbers
- [ ] Changelog is available in launch metadata JSON and working memory
- [ ] First session (no previous HEAD) gracefully returns empty changelog

## Owner and Effort

- **Owner:** TBD
- **Effort:** S
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Direct code change. Changelog appears on next session after the first.
- **Rollback:** git revert. Old session state files without prev_main_head are handled gracefully.
- **Blast radius:** Session startup only. Extra git operations add ~50ms.

## Test Plan

- [ ] Unit test: changelog computation with known commit range
- [ ] Unit test: empty changelog when no previous HEAD exists
- [ ] Integration test: two sequential sessions, second shows changes from first
- [ ] Manual verification: launch metadata includes changelog field

## Operational Impact

- **Metrics:** None.
- **Logging:** None.
- **Alerts:** None.
- **Disk/CPU/Memory:** One extra git log + git diff per session start (~50ms).

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Session changelog | P2 | S | Saves 3-5 tool calls per session, improves continuity |

## Trade-offs

Recording only main HEAD (not all branch HEADs) keeps the implementation simple. Agents working on feature branches can still see main-branch drift, which is the most common source of inter-session surprise. Branch-specific changelogs can be added later if needed.
