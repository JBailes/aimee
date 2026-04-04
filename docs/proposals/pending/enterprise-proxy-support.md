# Proposal: Enterprise Proxy Support with CA Bundle Management

## Problem

Aimee has no awareness of HTTPS proxy environments. In corporate/enterprise networks where outbound traffic goes through a proxy with custom CA certificates, aimee's HTTP calls fail with TLS errors. Users must manually configure proxy settings per-tool, which is error-prone.

This affects all HTTP-dependent features in both CLI and webchat: API calls, MCP connections, webhook deliveries.

The `soongenwong/claudecode` repo at `rust/crates/runtime/src/remote.rs` implements proxy bootstrap with CA bundle management and NO_PROXY lists.

## Goals

- Aimee detects and uses proxy configuration from standard environment variables (`HTTPS_PROXY`, `NO_PROXY`, etc.).
- Custom CA certificates are loaded from `SSL_CERT_FILE` / `REQUESTS_CA_BUNDLE`.
- Known-safe hosts (anthropic.com, github.com) are automatically added to `NO_PROXY`.
- Proxy settings are injected into subprocess environments (delegate bash commands, MCP server processes).
- Works transparently for CLI and webchat.

## Approach

### Environment Variables

Read proxy config from:
```
HTTPS_PROXY / https_proxy
NO_PROXY / no_proxy
SSL_CERT_FILE
NODE_EXTRA_CA_CERTS
REQUESTS_CA_BUNDLE
CURL_CA_BUNDLE
```

### Default NO_PROXY Hosts

```c
static const char *NO_PROXY_HOSTS[] = {
    "localhost", "127.0.0.1", "::1",
    "169.254.0.0/16", "10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16",
    "anthropic.com", "*.anthropic.com",
    "github.com", "api.github.com", "*.github.com",
    NULL
};
```

### Changes

| File | Change |
|------|--------|
| `src/proxy_bootstrap.c` (new) | Proxy detection, CA bundle resolution, NO_PROXY merging, subprocess env injection |
| `src/headers/proxy_bootstrap.h` (new) | Public API |
| `src/agent_http.c` | Apply proxy config to HTTP client |
| `src/agent_tools.c` | Inject proxy env vars into subprocess environments |
| `src/config.c` | Add optional proxy overrides in config |

## Acceptance Criteria

- [ ] `HTTPS_PROXY` is detected and used for API calls
- [ ] `NO_PROXY` is merged with default safe hosts
- [ ] Custom CA bundles from `SSL_CERT_FILE` are loaded
- [ ] Subprocess environments (delegates, MCP servers) inherit proxy settings
- [ ] Works for CLI and webchat HTTP calls

## Owner and Effort

- **Owner:** delegate (code)
- **Effort:** S-M (1-2 days)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Reads existing env vars — zero config needed. Falls back to direct connections if no proxy env is set.
- **Rollback:** Remove proxy detection — HTTP calls go direct.
- **Blast radius:** Wrong proxy config could break connectivity. Mitigation: log proxy detection at INFO.

## Test Plan

- [ ] Unit tests: env var parsing, NO_PROXY merging, CA bundle path resolution
- [ ] Integration tests: mock proxy server, verify requests routed through it
- [ ] Manual verification: set `HTTPS_PROXY`, run delegate, confirm traffic goes through proxy

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Proxy detection + HTTP integration | P3 | S | High — unblocks enterprise users |
| CA bundle management | P3 | S | High — TLS errors are common blocker |
| Subprocess env injection | P3 | S | Medium |

## Source Reference

Implementation reference: `soongenwong/claudecode` at `rust/crates/runtime/src/remote.rs`.
