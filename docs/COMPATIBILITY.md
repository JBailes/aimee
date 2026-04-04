# Compatibility Reference

This document summarizes the environments, integrations, and external interfaces supported by aimee.

Status terms used throughout this document:

- Tested: Confirmed through direct use on the listed platform, tool, or provider.
- Expected: Not verified as part of routine testing, but expected to work based on matching protocols, compatible toolchains, or equivalent runtime behavior.

## Primary Agent Support

These are the primary AI coding tools that aimee integrates with for memory injection, guardrails, and session management.

| Primary agent | Integration points | Tested versions | Support level | Notes |
|---|---|---:|---|---|
| Claude Code | `SessionStart`, `PreToolUse`, `PostToolUse`, `SessionEnd` | 1.x | Full | Full hook coverage for session lifecycle and tool interception. |
| Gemini CLI | `BeforeTool`, `AfterTool`, `SessionStart` | 2.x | Full | Uses Gemini CLI hook model rather than Claude-style hook names. |
| Codex CLI | `PreToolUse`, `PostToolUse`, `SessionStart`, local plugin, MCP | 1.x | Full | Supports both hook-based integration and MCP-backed tooling. |

## Platform Support

### Operating Systems

| Platform | Architecture | Build status | Runtime status | Notes |
|---|---|---|---|---|
| Debian 13 (trixie) | x86_64 | Tested | Tested | Primary development platform. |
| Ubuntu 22.04 and newer | x86_64 | Expected | Expected | Expected to work with the same general toolchain as Debian. |
| Proxmox VE 8.x | x86_64 | Tested | Tested | Deployed in production. |
| macOS 14 and newer | arm64 | Expected | Expected | Requires Homebrew-managed dependencies. |
| Windows via WSL2 | x86_64 | Expected | Expected | Native Windows is not supported. |

### Shell Environments

| Shell | Hook execution status | Notes |
|---|---|---|
| `bash` | Tested | Primary shell environment. |
| `zsh` | Expected | Common default shell for Claude Code on macOS. |
| `sh` (`dash`) | Expected | Minimal POSIX shell environment. |

## Build Dependencies

These dependencies are required to build or run major aimee components.

| Dependency | Minimum version | Debian package | Required for | Notes |
|---|---:|---|---|---|
| `gcc` | 10+ | `build-essential` | All binaries | Required C compiler toolchain. |
| `make` | 4.0+ | `build-essential` | Build system | Used for project build orchestration. |
| SQLite3 | 3.35+ with FTS5 | `libsqlite3-dev` | All components except the thin client | FTS5 is required for memory full-text search. |
| `libcurl` | 7.68+ | `libcurl4-openssl-dev` | Delegate agent HTTP, server | Required for outbound HTTP integrations. |
| OpenSSL | 1.1+ | `libssl-dev` | Webchat TLS | Provides TLS support. |
| PAM | System version | `libpam0g-dev` | Webchat authentication | Uses the host platform PAM implementation. |

### SQLite Requirement

FTS5 support is required for full-text memory search. Most system SQLite packages include FTS5, but it should be verified on minimal or custom builds.

Example check:

```bash
sqlite3 :memory: "SELECT fts5();" 2>&1 | grep -q "wrong number" && echo "FTS5 available"
```

## Delegate Providers

These are the API providers that delegate agents can connect to for task offloading. The primary agent does not use these providers directly; primary-agent integration happens through hooks and MCP.

| Provider | API format | Authentication | Models tested | Notes |
|---|---|---|---|---|
| OpenAI | `/chat/completions` | Bearer token | `gpt-4o`, `gpt-4o-mini` | OpenAI-compatible chat completions support. |
| ChatGPT (Codex) | `/backend-api/codex/responses` | OAuth device flow | `gpt-5.4`, `gpt-5.4-mini` | Uses Codex-specific backend API format. |
| Anthropic | `/v1/messages` | `x-api-key` header | `claude-sonnet-4-6`, `claude-haiku-4-5` | Native Anthropic messages API support. |
| Google (Gemini) | `/v1beta/openai` | OAuth or API key | `gemini-2.5-flash`, `gemini-2.5-flash-lite` | Uses Google’s OpenAI-compatible endpoint surface. |
| Ollama | `/v1/chat/completions` | None | `llama3.2`, any local model | Local provider with no built-in auth requirement. |
| Groq | `/openai/v1` | Bearer token | `llama-3.3-70b-versatile` | OpenAI-compatible API variant. |

## MCP Protocol

The MCP server (`aimee mcp-serve`) exposes aimee knowledge and actions to primary agents that support MCP. It is built into the `aimee` client binary.

| MCP capability | Version or scope | Status | Details |
|---|---|---|---|
| JSON-RPC 2.0 | `2024-11-05` | Implemented | Core RPC protocol support. |
| `stdio` transport | Standard input/output | Implemented | Primary supported transport. |
| `tools/list` | Tool discovery | Implemented | Exposes `search_memory`, `list_facts`, `get_host`, `list_hosts`, `find_symbol`, `delegate`, `preview_blast_radius`, `record_attempt`, `list_attempts`, and `delegate_reply`. |
| `tools/call` | Tool invocation | Implemented | Full tool execution support. |
| `resources/*` | MCP resources | Implemented | Resource endpoints are available. |
| `prompts/*` | MCP prompts | Implemented | Prompt endpoints are available. |
| HTTP SSE transport | Server-sent events | Not implemented | No HTTP transport support at this time. |

## Client Registration

This section covers how the aimee MCP server is registered with supported clients.

| Client | Registration method | Status | Details |
|---|---|---|---|
| Claude Code | Workspace-local `.mcp.json` written by `aimee init` or `aimee setup` | Implemented | Native local registration flow is available. |
| Codex CLI | Local marketplace entry in `~/.agents/plugins/marketplace.json`, mirrored plugin payload in `~/plugins/aimee`, `~/.agents/plugins/plugins/aimee`, and `~/.codex/plugins/cache/local/aimee`, plus activation in `~/.codex/config.toml` | Implemented | Registration requires multiple local plugin/cache locations. |
| Gemini CLI | MCP binary install only | Partial | No client-native registration file is written yet. |
| GitHub Copilot | MCP binary install only | Partial | No client-native registration file is written yet. |

## Known Limitations

| Area | Limitation | Impact |
|---|---|---|
| Native Windows | Native Windows is not supported. | Windows users must run under WSL2. |
| macOS packaging | Homebrew-managed dependencies are required. | Manual dependency setup is needed outside Debian-like environments. |
| SQLite builds | FTS5 must be present. | Memory full-text search is unavailable without FTS5. |
| Gemini CLI registration | No client-native registration is written yet. | MCP binary may need to be installed or configured manually. |
| GitHub Copilot registration | No client-native registration is written yet. | MCP binary may need to be installed or configured manually. |
| MCP transport | HTTP SSE transport is not implemented. | MCP support is limited to available implemented transports, primarily `stdio`. |
