# Proposal: State Export and Import

## Problem

All aimee state lives in a single SQLite database (`~/.config/aimee/aimee.db`)
plus config files. There is no way to:

1. **Move state to a new machine.** With macOS and Windows support planned, users
   will need to migrate their memories, rules, and configurations between
   platforms.
2. **Back up state portably.** Copying the raw `.db` file works but is fragile
   across schema versions and platforms (endianness, page size).
3. **Selectively transfer state.** A user might want to export just their
   memories or just their rules to share a configuration with another project.

## Goals

- Export all aimee state to a portable, human-readable format.
- Import on a new machine with automatic schema adaptation.
- Selective export by category (memories, rules, config, agents).

## Approach

### 1. Export command

`aimee export [--category <cat>] [--output <path>]`

Categories: `all` (default), `memories`, `rules`, `config`, `agents`, `decisions`.

Export format: a directory (or `.tar.gz` with `--archive`) containing:

```
aimee-export/
  manifest.json       # version, export date, categories, source platform
  memories.jsonl      # one JSON object per memory
  rules.jsonl         # one JSON object per rule
  decisions.jsonl     # one JSON object per decision
  config.json         # config (secrets redacted, key commands preserved)
  agents/             # agent config files
  style.json          # learned style preferences
```

JSONL (one object per line) for easy streaming, diffing, and partial import.

### 2. Import command

`aimee import <path> [--category <cat>] [--conflict <strategy>]`

Conflict strategies:
- `skip` (default): skip items that already exist (by key/ID)
- `overwrite`: replace existing items
- `merge`: for memories, merge content; for rules, keep both

Import process:
1. Read `manifest.json`, verify compatibility
2. For each category, read JSONL and insert/update via existing DB functions
3. Report: imported N memories, skipped M duplicates, etc.

Schema adaptation: since the format is logical (JSON objects with named fields),
not physical (raw SQLite rows), imports work across schema versions as long as
the field names are stable. Unknown fields are ignored. Missing fields use
defaults.

### 3. Secret redaction

Export redacts sensitive fields:
- `openai_key_cmd` is preserved (it's a command, not a key)
- Server token is excluded
- Any field matching known sensitive patterns is replaced with `"<redacted>"`

Import skips redacted fields, preserving the target machine's values.

### Changes

| File | Change |
|------|--------|
| `src/cmd_core.c` | Add `aimee export` and `aimee import` subcommands |
| `src/db.c` | Add `db_export_table()` and `db_import_table()` helpers |
| `src/cmd_table.c` | Register subcommands |

## Acceptance Criteria

- [ ] `aimee export` produces a directory with JSONL files for each category
- [ ] `aimee export --archive` produces a `.tar.gz`
- [ ] `aimee export --category memories` exports only memories
- [ ] `aimee import` loads exported state with conflict resolution
- [ ] Secrets are redacted in export, skipped on import
- [ ] Export from Linux imports successfully on macOS (when portability lands)

## Owner and Effort

- **Owner:** TBD
- **Effort:** S-M
- **Dependencies:** None (but becomes more valuable with macOS/Windows portability)

## Rollout and Rollback

- **Rollout:** New subcommands available immediately.
- **Rollback:** Revert commit. Export files on disk remain valid.
- **Blast radius:** Import with `overwrite` strategy can replace existing data. Default `skip` is safe.

## Test Plan

- [ ] Unit test: export produces valid JSONL for each category
- [ ] Unit test: import with `skip` does not overwrite existing entries
- [ ] Unit test: import with `overwrite` replaces existing entries
- [ ] Integration test: round-trip export→import preserves all data
- [ ] Integration test: secrets redacted in export, preserved on import target
- [ ] Manual: export on one machine, import on another

## Operational Impact

- **Metrics:** None.
- **Logging:** Import/export summary logged (counts per category).
- **Alerts:** None.
- **Disk/CPU/Memory:** Export size proportional to database content. Typical: <10MB.

## Priority

P2 — becomes important when cross-platform support lands.

## Trade-offs

**Why JSONL instead of raw SQLite dump?** JSONL survives schema changes, is
human-readable, and is platform-independent. SQLite dumps are faster but
brittle across versions.

**Why not incremental sync?** Incremental sync requires change tracking (CDC),
conflict resolution policies, and a sync protocol. Full export/import is
simpler and covers the actual use case (migration, not continuous sync).
