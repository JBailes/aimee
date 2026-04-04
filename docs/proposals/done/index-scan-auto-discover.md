# Proposal: Index Scan Auto-Discovery

## Problem

`aimee index scan` with no arguments requires an `aimee.workspace.yaml` manifest to exist. If no manifest is found, it fatals with "no workspace manifest found." Users must manually create a manifest before they can scan, even though the directory structure already contains all the information needed (git repos as subdirectories).

Additionally, the scan does not register discovered projects in config.json workspaces, so they are not available for workspace-scoped features (worktrees, memory recall) until separately provisioned.

## Goals

- `aimee index scan` with no arguments discovers and indexes all git repos under the current directory, with or without a manifest.
- Discovered projects are registered in config.json workspaces automatically.
- If a manifest exists, it is still respected and new repos are added to it.
- New git repos discovered alongside manifest projects are added to the manifest automatically.

## Approach

Modify `idx_scan` in `cmd_index.c` to discover git repos alongside manifest loading:

1. Try `workspace_load()` as today.
2. If manifest exists, scan its projects, then also discover any new git repos in the workspace root not yet in the manifest. New repos are added to the manifest via `workspace_add_project()`.
3. If no manifest, scan CWD subdirectories for `.git/` directories.
4. Register each discovered project path in config.json workspaces (deduplicating against existing entries).

### Changes

| File | Change |
|------|--------|
| `src/cmd_index.c` | Add directory-scanning fallback in `idx_scan` when no manifest found |
| `src/cmd_index.c` | Add config registration for discovered projects |

## Acceptance Criteria

- [ ] `aimee index scan` from a directory with git subdirs (no manifest) indexes all projects
- [ ] `aimee index scan` from a directory with a manifest still uses the manifest
- [ ] Discovered projects are added to config.json workspaces (no duplicates)
- [ ] Non-git subdirectories are silently skipped

## Owner and Effort

- **Owner:** aimee
- **Effort:** S
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Code change, no migration needed.
- **Rollback:** Revert commit. Existing manifests still work.
- **Blast radius:** Only affects `aimee index scan` with no arguments and no manifest.

## Test Plan

- [ ] Unit tests: extend test_index.c with a test for directory-based discovery
- [ ] Manual verification: run `aimee index scan` from ~/aimee without manifest, confirm all repos indexed

## Operational Impact

- **Metrics:** None
- **Logging:** Prints each discovered project name during scan
- **Alerts:** None
- **Disk/CPU/Memory:** Negligible

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Auto-discovery fallback | P2 | S | Medium |

## Trade-offs

Could generate a manifest file automatically on discovery, but that adds write-side complexity for a read-only operation. The scan should just work without creating files beyond the index DB and config registration.
