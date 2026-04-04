# Proposal: Write-Before-Read Guard

## Problem

Agents sometimes use the Write tool to overwrite existing files they haven't read in the current session. This silently destroys existing content — the agent writes what it thinks the file should contain based on memory or assumption, which is often incomplete or wrong. The correct pattern is to Read first, then Edit. Write should only be used for new files.

Evidence: oh-my-openagent implements this guard in `src/hooks/write-existing-file-guard/hook.ts`. It tracks which files each session has Read and blocks Write calls to existing files that haven't been Read, returning "File already exists. Use edit tool instead."

## Goals

- Prevent Write to existing files that haven't been Read in the current session
- Allow Write to new files (file doesn't exist yet)
- Allow Write to files that have been Read (explicit overwrite intent)
- Protect delegate sessions from silent content destruction

## Approach

Maintain a per-session set of files that have been Read. In the MCP tool layer, intercept Write calls: if the target file exists and hasn't been Read in this session, return an error message instead of executing the write.

### Changes

| File | Change |
|------|--------|
| `src/mcp_tools.c` | Track Read calls per session; guard Write calls against unread existing files |
| `src/server_session.c` | Add `read_files` set to session state, cleared on session end |

## Acceptance Criteria

- [ ] Write to existing file without prior Read returns error with guidance
- [ ] Write to new (non-existent) file proceeds normally
- [ ] Write to file that was Read in this session proceeds normally
- [ ] Read tracking is per-session, not global
- [ ] Session cleanup frees the tracking set

## Owner and Effort

- **Owner:** aimee
- **Effort:** S (< 1 day)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Compile-time inclusion, always active
- **Rollback:** Remove the guard; Write proceeds unconditionally as before
- **Blast radius:** Only affects Write tool on existing files; all other tools unaffected

## Test Plan

- [ ] Unit test: Write to existing file without Read → blocked
- [ ] Unit test: Write to non-existent file → allowed
- [ ] Unit test: Read then Write to same file → allowed
- [ ] Unit test: session cleanup frees tracking state
- [ ] Integration test: delegate attempts blind overwrite, verify blocked

## Operational Impact

- **Metrics:** Count of blocked writes per session
- **Logging:** Log warning when a write is blocked
- **Disk/CPU/Memory:** One hash set per session, typically <100 entries

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Write-Before-Read Guard | P0 | S | High — prevents silent content destruction |

## Trade-offs

Alternative: always allow Write but emit a warning. Rejected because the damage from a blind overwrite is severe and irreversible (within the session). Blocking with a clear message is the safer default.

Inspiration: oh-my-openagent `src/hooks/write-existing-file-guard/hook.ts`
