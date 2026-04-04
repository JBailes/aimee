# Proposal: OAuth PKCE Client Flow for Remote MCP Servers

## Problem

Aimee acts as an MCP server (`mcp_server.c`) but has limited support for connecting as an MCP *client* to remote servers. Many cloud-hosted MCP servers require OAuth 2.0 authentication. Without a client-side OAuth flow, aimee cannot consume remote MCP tools (GitHub MCP, Google Drive MCP, enterprise MCP gateways).

This affects CLI and webchat equally — any session wanting remote MCP tools.

The `soongenwong/claudecode` repo implements OAuth PKCE at `rust/crates/runtime/src/oauth.rs` and multi-transport MCP client config at `rust/crates/runtime/src/mcp_client.rs`.

## Goals

- Aimee can connect to remote MCP servers with OAuth 2.0 authentication.
- OAuth PKCE flow handles authorization, callback, token exchange, and refresh.
- Tokens are persisted securely and refreshed automatically.
- MCP client supports Stdio and SSE transports.
- CLI uses browser-based auth flow; webchat uses redirect-based auth flow.

## Approach

### OAuth PKCE Flow

```
1. Generate code_verifier + code_challenge (S256)
2. Build authorization URL
3. CLI: open browser, start local callback listener
   Webchat: redirect user, handle callback via webchat server
4. Exchange code + code_verifier for tokens
5. Store in secret_store, auto-refresh on expiry
```

### MCP Client Config

```json
{
  "mcp_clients": [
    {
      "name": "github-tools",
      "transport": "sse",
      "url": "https://mcp.github.com/sse",
      "auth": { "type": "oauth", "client_id": "...", "scopes": ["repo"] }
    },
    {
      "name": "local-tools",
      "transport": "stdio",
      "command": "my-mcp-server",
      "auth": null
    }
  ]
}
```

### Changes

| File | Change |
|------|--------|
| `src/oauth_client.c` (new) | PKCE flow: verifier/challenge generation, callback listener, token exchange/refresh |
| `src/mcp_client.c` (new) | MCP client: connect via stdio/SSE, tool discovery, tool calls |
| `src/secret_store.c` | Store/retrieve OAuth tokens per MCP server |
| `src/config.c` | Add `mcp_clients` config section |
| `src/mcp_tools.c` | Merge remote MCP tools into aimee's tool registry |
| `src/webchat.c` | Handle OAuth callback redirect for webchat auth flows |

## Acceptance Criteria

- [ ] Aimee connects to a remote SSE-based MCP server with OAuth
- [ ] CLI opens browser for auth; webchat redirects
- [ ] Tokens persisted and reused across sessions
- [ ] Expired tokens auto-refreshed
- [ ] Remote tools appear in tool registry alongside local tools
- [ ] Works in both CLI and webchat

## Owner and Effort

- **Owner:** delegate (code)
- **Effort:** L (3-5 days)
- **Dependencies:** None, complements plugin-system proposal

## Rollout and Rollback

- **Rollout:** Config-gated — no `mcp_clients` means no remote connections.
- **Rollback:** Remove config section.
- **Blast radius:** OAuth misconfiguration could fail to authenticate. Clear error messages mitigate.

## Test Plan

- [ ] Unit tests: PKCE generation, auth URL construction, token parsing, refresh
- [ ] Integration tests: mock OAuth server + mock MCP server, full flow
- [ ] Manual verification: configure a real remote MCP server, authenticate, call a tool

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| MCP client (stdio) | P2 | M | High — local MCP servers |
| MCP client (SSE) | P2 | M | High — remote MCP servers |
| OAuth PKCE flow | P3 | M | Medium — for authenticated remotes |
| Token persistence + refresh | P3 | S | Medium — usability |

## Source Reference

Implementation reference: `soongenwong/claudecode` at `rust/crates/runtime/src/oauth.rs` and `rust/crates/runtime/src/mcp_client.rs`.
