# Proposal: Capability Policy Centralization and Deny-by-Default Method Registry

## Problem

Method-to-capability mapping is defined in `server_capability_for_method()`
(`server_auth.c:125-177`) as a series of `strcmp`/`strncmp` checks. Issues:

1. **No single source of truth:** The mapping is procedural code, not a declarative
   table. Adding a new RPC method requires remembering to add a capability check.
   Forgetting silently defaults to `CAPS_ALL` (highest privilege), which is safe
   but unauditable.
2. **No compile-time enforcement:** New methods can be added to the dispatch table
   (`server.c`) without a corresponding capability entry — there is no test that
   detects this drift.
3. **No runtime introspection:** When a request is denied, the error message is
   generic ("unauthorized"). Operators cannot see *which* capability was required
   or *why* the principal lacked it.

## Goals

- Single declarative table defines method → capability for both server and tests.
- Tests fail when new methods are introduced without explicit capability policy.
- Denied operations include machine-readable reason (principal, required cap, decision).

## Approach

### 1. Declarative method registry table

Replace the procedural `server_capability_for_method()` with a static table:

```c
typedef struct {
    const char *method;         /* exact match or prefix with trailing '*' */
    uint32_t required_caps;     /* capability bitmask */
    const char *description;    /* human-readable for audit */
} method_policy_t;

static const method_policy_t method_registry[] = {
    {"memory.store",    CAP_MEMORY_WRITE,  "store memory"},
    {"memory.search",   CAP_MEMORY_READ,   "search memories"},
    {"memory.*",        CAP_MEMORY_READ,   "memory operation"},
    {"tool.execute",    CAP_TOOL_EXECUTE,  "execute tool"},
    {"delegate",        CAP_DELEGATE,      "delegate task"},
    /* ... */
    {NULL, 0, NULL}  /* sentinel */
};
```

Lookup: scan table for exact match first, then prefix match. If no match,
require `CAPS_ALL` (deny-by-default for unknown methods).

### 2. Registry completeness test

Add a test that:
1. Extracts all method strings from `server_dispatch()` (or a parallel dispatch table).
2. Verifies each appears in `method_registry[]`.
3. Fails with a clear message naming the unregistered method.

This can be a compile-time static assert if both tables are defined as macros,
or a runtime test that iterates both tables.

### 3. Structured authz decision records

Enhance `server_send_error()` for auth failures:

```json
{
    "status": "error",
    "code": "AUTHZ_DENIED",
    "principal": "uid:1000",
    "required_caps": ["CAP_MEMORY_WRITE"],
    "held_caps": ["CAP_MEMORY_READ", "CAP_DASHBOARD_READ"],
    "method": "memory.store"
}
```

Log the same information to stderr for operator forensics.

### Changes

| File | Change |
|------|--------|
| `src/server_auth.c` | Replace procedural mapping with `method_registry[]` table lookup |
| `src/server_auth.h` | Export `method_policy_t` and `method_registry[]` for test access |
| `src/server.c` | Use structured authz error in dispatch |
| `src/tests/` | Add registry completeness test comparing dispatch methods against registry |

## Acceptance Criteria

- [ ] All RPC methods are listed in `method_registry[]` with explicit capabilities
- [ ] Adding a new dispatch method without a registry entry fails the test suite
- [ ] Authz denials include structured reason (principal, required caps, held caps, method)
- [ ] Existing auth behavior is unchanged for all current methods

## Owner and Effort

- **Owner:** TBD
- **Effort:** S-M
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Ships with next binary. No config changes.
- **Rollback:** Revert commit. Restores procedural capability mapping.
- **Blast radius:** If the table has a typo (wrong method string), that method defaults to `CAPS_ALL` — same as current behavior for unknown methods. No security regression possible.

## Test Plan

- [ ] Unit test: each registered method resolves to correct capability
- [ ] Unit test: unregistered method requires `CAPS_ALL`
- [ ] Completeness test: all dispatch methods present in registry
- [ ] Integration test: denied request returns structured error with correct fields
- [ ] Manual: add a new dispatch method, verify test fails without registry entry

## Operational Impact

- **Metrics:** None new.
- **Logging:** Authz denials now include structured detail (principal, caps, method).
- **Alerts:** None.
- **Disk/CPU/Memory:** Negligible — table lookup is O(n) with ~20 methods. Faster than current strcmp chain.

## Priority

P0 — prevents silent authorization drift as methods are added.

## Trade-offs

**Why a static table instead of config file?** Capability policy is a compile-time
security invariant, not an operator tuning knob. Hardcoding prevents
misconfiguration.

**Why not a hash map?** With ~20 methods, linear scan is faster than hash
overhead. A hash map adds complexity for no measurable benefit.
