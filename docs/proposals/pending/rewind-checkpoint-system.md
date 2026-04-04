# Proposal: Rewind/Checkpoint System for Session Recovery

## Problem

When an agent (delegate or primary) makes a destructive mistake — overwrites a file incorrectly, deletes something it shouldn't have, or goes down a bad path — there is no way to roll back to a known-good state. The user must manually reconstruct the correct file state using git, editor undo, or memory. This is especially painful during long multi-turn sessions where the bad change happened several turns ago.

Git commits are too coarse for this: agents don't commit after every tool call, and `git stash`/`git checkout` discards all changes, not just the bad ones. Users need conversation-level granularity — "undo back to turn 12 and try again."

Inspired by mistral-vibe's `RewindManager`, which snapshots file state at checkpoints and restores it on rewind.

## Goals

- Users can rewind a session to any previous turn, restoring file state to what it was at that point.
- Checkpoints are created automatically before each tool call that modifies files.
- Rewind works in both CLI (`aimee rewind`) and webchat (button/API endpoint).
- File restoration is optional — users can rewind conversation context without touching files.

## Approach

Add a checkpoint system that captures file snapshots at key moments during the agent loop. Each checkpoint records:
- The turn number / message index
- A list of `(path, content_or_null)` pairs for every file touched since the previous checkpoint
- A timestamp

On rewind, the system truncates conversation history to the target turn and optionally restores files from the checkpoint. Files that didn't exist at the checkpoint are deleted; files that did exist are written back.

### Storage

Checkpoints are stored in the session's working memory (SQLite), keyed by session ID and turn number. File contents are stored as BLOBs to handle binary files. A configurable max checkpoint count (default 50) prevents unbounded growth — oldest checkpoints are pruned.

### Integration Points

- **Agent loop (`agent.c`)**: Before executing any file-writing tool call, create a checkpoint of the files about to be modified.
- **CLI (`cmd_core.c` or new `cmd_rewind.c`)**: `aimee rewind [turn]` — list checkpoints or rewind to a specific turn.
- **Webchat (`webchat.c`)**: SSE event for checkpoint creation; POST endpoint for rewind action.
- **Guardrails (`guardrails.c`)**: Record which files each tool call touches so checkpoints know what to snapshot.

### Changes

| File | Change |
|------|--------|
| `src/headers/checkpoint.h` | New: checkpoint types, API |
| `src/checkpoint.c` | New: checkpoint create/restore/prune/list |
| `src/agent.c` | Hook checkpoint creation before file-modifying tool calls |
| `src/cmd_core.c` | Add `rewind` subcommand |
| `src/webchat.c` | Add checkpoint SSE events and rewind API endpoint |
| `src/webchat_assets.c` | Add rewind UI button/controls to webchat frontend |
| `src/db.c` | Add `checkpoints` and `checkpoint_files` tables |

## Acceptance Criteria

- [ ] `aimee rewind list` shows checkpoints with turn number, timestamp, and files touched
- [ ] `aimee rewind <turn>` restores files and truncates conversation to that turn
- [ ] `aimee rewind <turn> --no-restore` truncates conversation without touching files
- [ ] Webchat exposes rewind via a clickable checkpoint marker on each turn
- [ ] Checkpoints auto-prune when count exceeds configured max
- [ ] Round-trip test: modify 3 files across 5 turns, rewind to turn 2, verify files match turn-2 state

## Owner and Effort

- **Owner:** aimee
- **Effort:** M (2-3 days)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** New DB tables auto-created on upgrade. Feature is opt-in via `aimee rewind`.
- **Rollback:** Drop checkpoint tables. No other state affected.
- **Blast radius:** Only affects sessions that use rewind. No impact on normal operation.

## Test Plan

- [ ] Unit tests: checkpoint create, restore, prune logic
- [ ] Integration tests: multi-turn agent session with rewind at various points
- [ ] Failure injection: rewind when target file is locked or deleted externally
- [ ] Manual verification: webchat rewind button works end-to-end

## Operational Impact

- **Metrics:** `checkpoint_created`, `checkpoint_rewound` counters
- **Logging:** INFO on checkpoint create/rewind, WARN on restore failures
- **Alerts:** None
- **Disk/CPU/Memory:** Moderate disk for file snapshots (mitigated by prune limit). No CPU/memory impact during normal operation.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Rewind/Checkpoint System | P2 | M | High — major UX improvement for error recovery |

## Trade-offs

**Alternative: Git-based checkpoints.** Use `git stash` or temporary commits instead of storing file contents in SQLite. Rejected because: (a) not all workspaces are git repos, (b) pollutes git history, (c) can't handle partial restores.

**Alternative: OS-level filesystem snapshots (btrfs/ZFS).** Too heavy, not portable, and not conversation-aware.

**Known limitation:** Binary files stored as BLOBs may bloat the session DB for large files. Mitigated by the prune limit and by only snapshotting files the agent actually touches.
