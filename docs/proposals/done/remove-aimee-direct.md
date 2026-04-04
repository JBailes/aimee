# Proposal: Remove aimee-direct Binary

## Problem

The codebase builds three separate binaries: `aimee` (CLI client), `aimee-server`, and `aimee-direct`. The `aimee-direct` binary is a monolith that links all command handlers directly. It exists solely because `server_forward.c` forks and exec's it to handle `cli.forward` requests.

This architecture adds complexity:
- Two binaries (`aimee` and `aimee-direct`) share the same command table but are built differently
- The fork+exec path in `server_forward.c` has to locate the `aimee-direct` binary at runtime
- Session state (like session ID) must be passed via env vars across the exec boundary
- Build system has separate targets, link rules, and install steps for both

The server should handle commands directly in forked children without exec'ing a separate binary.

## Goals

- Remove the `aimee-direct` binary entirely
- `cli.forward` forks a child that runs the command handler in-process (no exec)
- Session ID and other server state are directly accessible in the child (inherited via fork)
- Only three artifacts: `aimee` (client), `aimee-server`, `aimee-mcp` (or merged further)

## Approach

### 1. Inline command execution in server_forward.c

Replace the fork+exec pattern with fork-only:
- Child process calls the command handler directly via the command dispatch table
- No need for `execvp("aimee-direct", ...)` -- just call `cmd_dispatch(cmd_name, argc, argv)`
- Stdout/stderr are already redirected to the capture pipe

### 2. Handle process isolation

The fork gives us:
- Separate address space (command handlers can't corrupt server state)
- Inherited file descriptors (db connections, session state)
- Inherited env vars (no need for setenv dance)
- Clean process exit (even if command crashes, server survives)

Potential issues to address:
- Command handlers that call `exit()` or `fatal()` -- these are fine in a forked child
- SQLite connections -- each child should open its own (current aimee-direct already does this)
- Signal handlers -- reset in child to defaults

### 3. Update build system

- Remove `DIRECT` target from Makefile
- Remove `DIRECT_SRCS` and `DIRECT_OBJS`
- Link command libraries into server binary (they're already in `.a` archives)
- Update install target

### Changes

| File | Change |
|------|--------|
| src/server_forward.c | Replace fork+exec with fork+cmd_dispatch |
| src/Makefile | Remove aimee-direct target, link cmd libs into server |
| src/main.c | Remove aimee-direct entry point code if separate |
| install.sh | Remove aimee-direct installation |

## Acceptance Criteria

- [ ] `aimee-direct` binary is no longer built
- [ ] `aimee work claim` / `aimee work complete` work through server forwarding
- [ ] `aimee delegate` works through server forwarding
- [ ] Session ID is available in forwarded commands without env var propagation
- [ ] All existing tests pass
- [ ] Server remains stable when command handlers crash

## Owner and Effort

- **Owner:** aimee core
- **Effort:** M
- **Dependencies:** None
- **Priority:** P2

## Rollout and Rollback

- **Rollout:** Single commit removing the binary and updating the forward path.
- **Rollback:** git revert. The aimee-direct code can be restored from history.

## Test Plan

- [ ] All unit tests pass without aimee-direct
- [ ] Integration tests: forwarded commands produce correct output
- [ ] Stress test: rapid sequential commands don't cause resource leaks
- [ ] Crash test: command that calls fatal() doesn't take down server

## Operational Impact

- One fewer binary to build, install, and maintain
- Slightly faster command execution (no exec overhead)
- Simpler session ID propagation (no env var needed)
