# Proposal: Reduce HTTP Client Boilerplate in agent_http.c

## Problem

`agent_http.c` contains three HTTP POST functions that share ~80% identical libcurl setup code:

1. `agent_http_post` (lines 196-256): Standard JSON POST with response buffer
2. `agent_http_post_form` (lines 258-300): Form-encoded POST with response buffer
3. `agent_http_post_stream` (lines 319-373): JSON POST with streaming callback

All three repeat the same ~15 `curl_easy_setopt` calls for: URL, POST method, body, headers, timeout, `CURLOPT_NOSIGNAL`, user agent, SSL verify peer, SSL verify host, write function, and write data. The extra-header parsing loop (`strtok_r` on newlines) is copy-pasted between `agent_http_post` and `agent_http_post_stream` (lines 214-226 and 335-347).

Additionally, `agent_http_post_form` hardcodes `originator: codex_cli_rs` as a header (line 270) — a leftover from the codex CLI integration that should be configurable or documented.

## Goals

- Common curl setup extracted into a shared helper
- Extra-header parsing exists once
- Each POST variant is <20 lines of specialization
- No behavioral changes

## Approach

Extract a static `setup_curl()` helper:

```c
static CURL *setup_curl(const char *url, struct curl_slist **headers_out,
                        const char *content_type, const char *auth_header,
                        const char *body, int timeout_ms, const char *extra_headers)
```

This function:
- Initializes curl
- Sets URL, POST, body, timeout, NOSIGNAL, user agent, SSL options
- Builds the header list (content_type + auth + extra headers via strtok_r)
- Returns the configured `CURL` handle and assembled header list

Each POST variant then just:
1. Calls `setup_curl()`
2. Sets its specific write callback / write data
3. Calls `curl_easy_perform()`
4. Extracts status code
5. Cleans up

### Changes

| File | Change |
|------|--------|
| `src/agent_http.c` | Add static `setup_curl()` helper (~30 lines) |
| `src/agent_http.c` | Rewrite `agent_http_post` to use `setup_curl()` (60 → ~20 lines) |
| `src/agent_http.c` | Rewrite `agent_http_post_form` to use `setup_curl()` (42 → ~20 lines) |
| `src/agent_http.c` | Rewrite `agent_http_post_stream` to use `setup_curl()` (54 → ~20 lines) |

## Acceptance Criteria

- [ ] `agent_http.c` Linux section reduced from ~220 to ~130 lines
- [ ] All three POST functions produce identical HTTP behavior (verified by `test_agent.c`)
- [ ] Extra-header parsing appears exactly once
- [ ] Common `curl_easy_setopt` calls appear exactly once

## Owner and Effort

- **Owner:** aimee core
- **Effort:** S
- **Dependencies:** none

## Rollout and Rollback

- **Rollout:** Compile and test
- **Rollback:** Revert commit
- **Blast radius:** All agent API calls. Medium risk — must verify all three code paths still work.

## Test Plan

- [ ] Unit tests: `test_agent.c` covers all three POST variants
- [ ] Integration tests: end-to-end delegation (exercises `agent_http_post`), codex auth flow (exercises `agent_http_post_form`), streaming chat (exercises `agent_http_post_stream`)
- [ ] Manual verification: confirm extra headers still work, form POST still includes `originator` header

## Operational Impact

- **Metrics:** None
- **Logging:** None
- **Disk/CPU/Memory:** No change

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Extract `setup_curl()` helper | P3 | S | Maintainability, reduces option drift risk |

## Trade-offs

Could also deduplicate the WinHTTP implementation (lines 26-128), but Windows support appears dormant (`agent_http_post_form` and `agent_http_post_stream` are stubs returning -1). Recommend deferring WinHTTP cleanup until Windows is actively supported.
