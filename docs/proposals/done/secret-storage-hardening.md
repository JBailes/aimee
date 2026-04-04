# Proposal: Optional OS Keyring for Secret Storage

## Problem

The `openai_key_cmd` pattern already avoids storing API keys directly, but the
server auth token (`~/.config/aimee/server.token`) is a plaintext file. On
systems with OS keyrings, this token could be stored more securely without any
user friction — the keyring is unlocked with the user's login session.

This becomes more relevant as aimee targets macOS (Keychain) and eventually
Windows (Credential Manager).

## Goals

- Optional OS keyring storage for the server auth token, with plaintext file fallback.
- Cross-platform: libsecret (Linux), Keychain (macOS), Credential Manager (Windows).

## Approach

### 1. Keyring abstraction

```c
typedef struct {
    int (*store)(const char *service, const char *key, const char *value);
    int (*load)(const char *service, const char *key, char *buf, size_t len);
    const char *name;
} secret_backend_t;
```

Backends compiled conditionally. Auto-detect at startup: try keyring, fall back
to file. Log which backend is active.

### 2. Automatic migration

On first startup with keyring available, migrate existing plaintext token into
keyring and replace file with sentinel `KEYRING:<backend>`.

### Changes

| File | Change |
|------|--------|
| `src/secret_store.c` | New: backend abstraction with libsecret, Keychain, and file backends |
| `src/secret_store.h` | New: backend API |
| `src/server_auth.c` | Use `secret_backend_t` for token load/store |

## Acceptance Criteria

- [ ] Keyring backend stores/loads tokens when available
- [ ] File backend used as fallback when keyring is unavailable
- [ ] Migration from file to keyring is automatic and logged

## Owner and Effort

- **Owner:** TBD
- **Effort:** S-M
- **Dependencies:** Windows/macOS portability (for non-Linux backends)

## Rollout and Rollback

- **Rollout:** File backend remains default. Keyring activates when available.
- **Rollback:** Revert commit. Tokens stay in keyring (accessible manually) or fall back to file.
- **Blast radius:** If keyring backend fails, file fallback ensures continuity.

## Test Plan

- [ ] Integration test: keyring backend round-trips a secret (platform-specific CI)
- [ ] Integration test: file fallback works when keyring unavailable
- [ ] Manual: verify migration from file to keyring

## Operational Impact

- **Metrics:** None.
- **Logging:** Active backend logged at startup.
- **Alerts:** None.
- **Disk/CPU/Memory:** One D-Bus/Keychain call instead of file read. Negligible.

## Priority

P2 — nice-to-have for cross-platform credential hygiene.

## Trade-offs

**Why optional?** Headless/container environments lack D-Bus. Mandating keyring
would break them.
