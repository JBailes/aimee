# Proposal: Config Load Optimization

## Owner

JBailes

## Problem

The aimee startup path calls `config_save(&cfg)` unconditionally at `main.c:168`
every time aimee starts. This rewrites the config file even when nothing changed,
which:

- Bumps the file mtime, defeating the mtime-based config cache in `config_load()`
- Wastes I/O on every startup
- Creates a TOCTOU window if two sessions start simultaneously

Additionally, `config_load()` is called 3 times during the monolith startup path
(main.c, cmd_session_start, cmd_launch). While the mtime cache should make
repeated loads cheap, the unconditional save busts the cache before subsequent
reads, forcing redundant disk I/O.

## Goals

- Config file is only written on first run (when it doesn't exist yet).
- The mtime cache in `config_load()` works correctly across the startup path.
- No behavioral change for users — defaults are still persisted on first run.

## Approach

Replace the unconditional `config_save(&cfg)` in `main.c` with a `stat()` check
on the config file path. If the file doesn't exist (first run), persist defaults.
Otherwise, skip the save entirely.

```c
config_load(&cfg);

/* Only persist config when the file doesn't exist yet (first run) */
{
   struct stat cfg_st;
   if (stat(config_default_path(), &cfg_st) != 0)
      config_save(&cfg);
}
```

The redundant `config_load()` calls in `cmd_session_start()` and `cmd_launch()`
are left as-is because: (a) the mtime cache now works properly, making them
effectively free; (b) changing function signatures would break the command
dispatch table.

## Acceptance Criteria

- `config_save()` is not called when config file already exists on disk.
- Config file mtime is unchanged after a normal startup (non-first-run).
- First-run behavior is preserved: defaults are written to disk.
- All existing tests pass.

## Test Plan

- Start aimee with an existing config file, verify mtime is unchanged afterward.
- Delete config file, start aimee, verify config file is created with defaults.
- Run the existing test suite to check for regressions.

## Rollback Plan

Revert the single commit. The only change is adding a `stat()` guard around
`config_save()`, so reverting restores the original unconditional save behavior.

## Operational Impact

- Reduces disk writes on every startup by 1 (the redundant config save).
- Fixes the mtime cache so subsequent `config_load()` calls in the startup path
  can return cached data instead of re-reading from disk.
- No new dependencies or configuration changes.
