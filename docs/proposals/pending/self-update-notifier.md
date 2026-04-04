# Proposal: Self-Update Notifier with Changelog Display

## Problem

When aimee is updated (via `git pull && make`), there is no indication of what changed. Users discover new features by accident or by reading commit logs. There is no version check, no changelog display, and no way to know if the running binary is stale.

Mistral-vibe implements a version cache (24h TTL), startup update check, and a "what's new" display that shows the changelog exactly once after each upgrade.

## Goals

- On startup, aimee checks if the running binary matches the latest built version and displays a notice if outdated.
- After an upgrade, the first invocation shows a concise "what's new" summary.
- The changelog is shown once per version, then suppressed.
- Works in both CLI (printed on startup) and webchat (banner at top of page).

## Approach

### Version Tracking

Embed a version string at compile time (`-DAIMEE_VERSION=$(git describe --tags --always)`). Store the "last seen version" in the DB (`key_value` table or a dedicated `version_state` row).

### Startup Check

On every CLI or webchat startup:
1. Compare `AIMEE_VERSION` against the stored "last seen" version.
2. If different: load `docs/WHATS_NEW.md`, display the section for the current version, then store the new version as "seen."
3. If same: skip.

### Stale Binary Detection

Optionally compare the binary's mtime against the repo's `HEAD` commit time. If the binary is older, print a one-line notice: `"aimee: binary is older than source — run 'make' to update"`.

### Changes

| File | Change |
|------|--------|
| `src/Makefile` | Embed `AIMEE_VERSION` at compile time |
| `src/main.c` | Version check on startup, changelog display |
| `src/db.c` | Add `version_state` key-value entry |
| `src/webchat.c` | Show update banner in webchat UI |
| `src/webchat_assets.c` | Add dismissible "what's new" banner component |
| `docs/WHATS_NEW.md` | New: per-version changelog in parseable format |

## Acceptance Criteria

- [ ] `aimee --version` prints the embedded version string
- [ ] First run after rebuild shows "what's new" content for the new version
- [ ] Second run after same rebuild does not show it again
- [ ] Stale binary detection prints a one-line warning when binary is older than HEAD
- [ ] Webchat shows a dismissible banner with the same content
- [ ] Version state persists across sessions in the DB

## Owner and Effort

- **Owner:** aimee
- **Effort:** S (1 day)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Automatic on rebuild. No config needed.
- **Rollback:** Remove version check from main.c. No persistent state impact.
- **Blast radius:** Cosmetic only — startup output.

## Test Plan

- [ ] Unit tests: version comparison, changelog section extraction
- [ ] Integration tests: rebuild with new version, verify display, verify suppression on second run
- [ ] Manual verification: webchat banner appearance and dismissal

## Operational Impact

- **Metrics:** None
- **Logging:** DEBUG for version check
- **Alerts:** None
- **Disk/CPU/Memory:** Negligible

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Self-Update Notifier | P3 | S | Low — quality-of-life improvement |

## Trade-offs

**Alternative: Check a remote registry.** Aimee is built from source, not a package. Git-based version comparison is the right model.

**Known limitation:** Requires `git describe` at build time. If building from a tarball without git metadata, falls back to a static version string.
