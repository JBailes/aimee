# Proposal: Structured Logging, Log Levels, and Security Audit Trail

## Problem

Multiple logging gaps across the codebase:

1. **No log levels.** Components print directly to stderr with no severity
   classification. Expected non-actionable messages (e.g., "no server found,
   starting new one") appear alongside actual errors, making it hard to filter
   in automation and CI.
2. **No security audit trail.** Security-relevant events (auth failures, CSRF
   denials, ACL blocks, sensitive path blocks, token file issues) are handled
   but not consistently emitted as structured, durable events.
3. **No structured format.** Log output is free-form text. Machine parsing
   requires fragile regex.

## Goals

- Global log level control (`ERROR`/`WARN`/`INFO`/`DEBUG`).
- Security events emitted as structured audit records.
- CI/tests run clean without expected-path noise.

## Approach

### 1. Log level infrastructure

```c
typedef enum {
    LOG_ERROR = 0,
    LOG_WARN  = 1,
    LOG_INFO  = 2,
    LOG_DEBUG = 3,
} log_level_t;

void aimee_log(log_level_t level, const char *module, const char *fmt, ...);
```

Output format (structured but human-readable):
```
2026-04-01T12:00:00Z ERROR server_auth: rate limited uid=1000 cooldown=300s
2026-04-01T12:00:00Z INFO  server: listening on /run/aimee/server.sock
2026-04-01T12:00:01Z DEBUG memory: search query="foo" results=3 elapsed_ms=2
```

Control via:
- `--log-level <level>` CLI flag
- `AIMEE_LOG_LEVEL` environment variable
- Default: `INFO`

### 2. Security audit events

Define specific security event types:

| Event | Level | Fields |
|-------|-------|--------|
| `auth_fail` | WARN | uid, ip, reason, failure_count |
| `auth_rate_limited` | WARN | uid, ip, cooldown_seconds |
| `authz_denied` | WARN | principal, method, required_caps, held_caps |
| `csrf_fail` | WARN | origin, expected_origin |
| `acl_denied` | WARN | source_ip, acl_rule |
| `sensitive_path_block` | INFO | path, severity, tool |
| `token_file_insecure` | ERROR | path, issue (mode/owner/symlink) |
| `token_rotated` | INFO | method (manual/ttl), connections_invalidated |

Security events are always logged regardless of log level (security audit
channel is separate from operational logging).

### 3. Audit log persistence

Security events written to an append-only file:
`~/.config/aimee/audit.log`

- Rotation: new file when size exceeds 10MB, keep last 5 files
- Format: one JSON object per line (machine-parseable)
- File permissions: 0600

**Platform note:** Log rotation uses `rename()` which is atomic on all target
platforms (Linux, macOS, Windows). File permissions use platform-appropriate
equivalents (POSIX `chmod` on Linux/macOS, ACL on Windows).

### 4. Migrate existing log calls

Replace `fprintf(stderr, ...)` calls across the codebase with `aimee_log()`
calls at appropriate levels. This is a large but mechanical change.

### Changes

| File | Change |
|------|--------|
| `src/log.c` | New: logging infrastructure with levels, formatting, rotation |
| `src/log.h` | New: log API, level enum, audit event types |
| `src/main.c` | Parse `--log-level` flag and `AIMEE_LOG_LEVEL` env |
| `src/server_auth.c` | Emit `auth_fail`, `auth_rate_limited`, `authz_denied` audit events |
| `src/webchat.c` | Emit `csrf_fail`, `acl_denied` audit events |
| `src/guardrails.c` | Emit `sensitive_path_block` audit events |
| `src/server.c` | Migrate `fprintf(stderr)` to `aimee_log()` |
| (all `src/*.c`) | Migrate remaining `fprintf(stderr)` calls |

## Acceptance Criteria

- [ ] `--log-level` and `AIMEE_LOG_LEVEL` control output verbosity
- [ ] Default log level is INFO — DEBUG messages suppressed
- [ ] Security events always logged to audit file regardless of log level
- [ ] Audit log is append-only, rotated at 10MB, last 5 files kept
- [ ] CI tests produce no unexpected stderr output at INFO level
- [ ] All `fprintf(stderr)` calls migrated to `aimee_log()`

## Owner and Effort

- **Owner:** TBD
- **Effort:** M-L (logging infrastructure is small; migration of all call sites is large)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Logging infrastructure ships with next binary. Call site migration can be incremental.
- **Rollback:** Revert commit. Restores `fprintf(stderr)` calls.
- **Blast radius:** Log level change may suppress messages users were relying on. Default INFO level preserves most current output.

## Test Plan

- [ ] Unit test: log level filtering works correctly
- [ ] Unit test: audit events written regardless of log level
- [ ] Unit test: log rotation triggers at size limit
- [ ] Integration test: `--log-level DEBUG` produces debug output
- [ ] Integration test: `--log-level ERROR` suppresses info/warn
- [ ] Integration test: CI test suite clean at INFO level

## Operational Impact

- **Metrics:** Audit event counts by type.
- **Logging:** This IS the logging proposal.
- **Alerts:** None.
- **Disk/CPU/Memory:** Audit log: 5 × 10MB = 50MB max. Logging overhead: negligible.

## Priority

P1 — foundational for debugging, security forensics, and CI hygiene.

## Trade-offs

**Why separate audit log instead of just log levels?** Security events must never
be accidentally suppressed by setting a high log level. A separate channel
ensures audit completeness independent of operational log verbosity.

**Why JSON lines for audit instead of structured binary?** JSON lines are human-
readable, greppable, and trivially parseable. Binary formats require tooling.
For a local tool, JSON lines are the right trade-off.
