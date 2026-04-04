# Proposal: Split webchat.c into Focused Modules

## Problem

webchat.c is 2619 lines, the largest file in the codebase by a wide margin. It handles TLS certificate generation, network ACL, session management, HTTP request parsing, SSE streaming, chat turn execution, embedded HTML templates, request routing, and the main server loop. This violates the Single Responsibility Principle and makes the file difficult to navigate, test, and modify safely. A change to the login page HTML requires reading past TLS crypto code. A session bug requires understanding the HTTP parser.

## Goals

- Every resulting file is under 1000 lines.
- Each file has a single, clear responsibility.
- No functional changes; pure structural refactoring.
- All existing tests and functionality preserved.

## Approach

Split webchat.c into 8 focused files plus a shared header:

### 1. webchat.c (~300 lines)
Main server loop: socket setup, connection accept, thread dispatch, cmd_webchat entry point.

### 2. webchat_tls.c (~120 lines)
`generate_self_signed_cert()`, TLS context setup, certificate/key management.

### 3. webchat_acl.c (~65 lines)
Network ACL loading from config file and IP checking (`acl_load`, `acl_check`).

### 4. webchat_session.c (~330 lines)
Session create/lookup/destroy, token generation, CSRF token management, session eviction policy. This is also where the session race condition (Bug 11 from bug-fixes-batch-1) would be fixed.

### 5. webchat_http.c (~130 lines)
HTTP parsing helpers: `url_decode`, `send_html`, `send_json`, `send_redirect`, `parse_form_field`, header extraction.

### 6. webchat_chat.c (~270 lines)
Chat turn execution, SSE event parsing, tool result handling, Claude CLI forwarding.

### 7. webchat_handler.c (~380 lines)
`handle_request()` refactored into a route dispatch table plus per-route handler functions (login, logout, chat, API endpoints, static assets).

### 8. webchat_html.c (~400 lines)
All embedded HTML template strings (login page, chat page, error pages). Currently these are large string literals inline in the handler.

### 9. headers/webchat_types.h (~140 lines)
All shared types (`wc_session_t`, `wc_turn_t`, etc.), constants (`WC_MAX_SESSIONS`, `CORS_ORIGIN_LEN`, etc.), and forward declarations used across webchat_*.c files.

### Changes

| File | Change |
|------|--------|
| `src/webchat.c` | Reduce to main loop + thread dispatch (~300 lines) |
| `src/webchat_tls.c` | New: TLS cert generation |
| `src/webchat_acl.c` | New: Network ACL |
| `src/webchat_session.c` | New: Session management |
| `src/webchat_http.c` | New: HTTP helpers |
| `src/webchat_chat.c` | New: Chat execution |
| `src/webchat_handler.c` | New: Request routing |
| `src/webchat_html.c` | New: HTML templates |
| `src/headers/webchat_types.h` | New: Shared types and constants |
| `src/Makefile` | Add new .o files to CMD layer |

## Acceptance Criteria

- [ ] webchat.c is under 300 lines
- [ ] No resulting file exceeds 1000 lines
- [ ] `make` builds clean with -Werror
- [ ] `make lint` passes (clang-format, cppcheck)
- [ ] Integration tests pass unchanged
- [ ] Manual test: webchat serves login page, chat works, HTTPS works

## Owner and Effort

- **Owner:** TBD
- **Effort:** M (mechanical move + adjust includes/statics, no logic changes)
- **Dependencies:** None (can land before or after bug-fixes-batch-1)

## Rollout and Rollback

- **Rollout:** Direct code change. No behavior change.
- **Rollback:** git revert (single commit).
- **Blast radius:** webchat command only. Server, CLI, hooks unaffected.

## Test Plan

- [ ] Integration tests pass unchanged
- [ ] Manual: `aimee webchat` starts, serves login, handles chat, HTTPS cert generated
- [ ] Manual: CORS origins respected, ACL blocks unauthorized IPs
- [ ] Build: all new .o files link correctly

## Operational Impact

- **Metrics:** None.
- **Logging:** None.
- **Alerts:** None.
- **Disk/CPU/Memory:** None. Pure structural change.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Split webchat.c | P2 | M | Maintainability, reviewability |

## Trade-offs

**Why 8 files instead of 3-4?** Fewer, larger files would still exceed 1000 lines. The split follows natural responsibility boundaries. Each file can be understood in isolation.

**Why a separate webchat_html.c?** HTML templates are large string literals that clutter handler logic. Separating them makes both the templates and the handlers easier to modify.

**Why not use a template engine?** Adds a dependency for minimal gain. The embedded strings are simple and rarely change. If dynamic templating is needed later, webchat_html.c is the obvious place to add it.
