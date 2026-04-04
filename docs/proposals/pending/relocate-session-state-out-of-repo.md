# Proposal: Relocate Session State Out of the Repository

## Problem

The aimee repository currently carries a large amount of local session state under repo-local directories such as `.claude/worktrees`. In the inspected checkout:

- `.claude` consumes roughly `244M`, while `src` is about `18M`
- `.claude/worktrees` contains 142 directories
- the repo shows roughly 8,492 files total, but only about 582 once `.git` and `.claude` are excluded

This is accidental bloat, not product complexity. It makes file search noisy, slows navigation, increases the chance of staging the wrong files, and makes the repository feel unmanageably large even when the actual maintained source tree is much smaller.

## Goals

- Move transient session and worktree artifacts out of the tracked repository tree.
- Keep the developer-visible repo focused on product source, tests, and docs.
- Preserve worktree/session isolation guarantees.
- Make cleanup and garbage collection of stale session artifacts explicit and predictable.

## Approach

Treat worktrees, chat/session scratch state, and other local execution artifacts as runtime state owned by aimee, not as project files.

Use a per-user state root outside the repository, for example:

- `~/.local/state/aimee/worktrees/<project>/...`
- `~/.cache/aimee/sessions/<project>/...`

The repository should contain only source-controlled files and minimal project metadata.

### Changes

| File | Change |
|------|--------|
| `src/workspace.c` | Resolve worktree/session paths from a state root outside the repo |
| `src/config.c` | Add configurable defaults for state and cache directories |
| `src/cmd_hooks.c` | Stop assuming repo-local session artifact paths |
| `src/cmd_work.c` | Read/write work queue state from the new runtime location as needed |
| `src/guardrails.c` | Update path classification logic for externalized state roots |
| `src/README.md` | Document the separation between repository files and runtime state |
| `docs/WORKSPACES.md` | Document new worktree/session storage layout and cleanup behavior |
| `install.sh` | Ensure state directories exist on first install/setup |

## Acceptance Criteria

- [ ] New sessions and worktrees are created outside the repository root by default.
- [ ] Existing workflows still get isolated worktrees and session state.
- [ ] `git status` in the repository remains clean during normal session activity.
- [ ] Garbage collection/pruning commands operate on the externalized state root.
- [ ] Existing session-start and worktree tests pass with the new path layout.

## Owner and Effort

- **Owner:** aimee core
- **Effort:** M
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Introduce the external state root behind a defaulted config setting, then migrate new sessions immediately. Optionally add one-time migration/help text for existing repo-local state.
- **Rollback:** Revert the path resolution change and continue using repo-local state.
- **Blast radius:** Session startup, worktree provisioning, cleanup, and any code that assumes repo-local runtime paths.

## Test Plan

- [ ] Unit tests: path resolution and config default tests for state-root selection.
- [ ] Integration tests: session start, worktree creation, stale worktree pruning.
- [ ] Failure injection: missing state directory, permission denied on state root, stale symlink/worktree references.
- [ ] Manual verification: start a session in a clean repo and confirm no `.claude/worktrees` growth under the project.

## Operational Impact

- **Metrics:** Add counters for external state-root creation and prune results.
- **Logging:** Log the resolved state root at debug level during session setup.
- **Alerts:** None.
- **Disk/CPU/Memory:** Repository disk usage drops; runtime disk usage is unchanged but moved to a managed location.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Externalize session/worktree state | P1 | M | High reduction in accidental repo bloat and day-to-day friction |

## Trade-offs

Moving state out of the repo adds one more path family to understand, but that is an explicit and healthy distinction between source and runtime data. An alternative is to keep repo-local state and improve ignore rules, but that does not solve navigation noise, accidental staging, or the perception that the source tree itself is much larger than it is.
