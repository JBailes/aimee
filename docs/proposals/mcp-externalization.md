# Proposal: MCP Tool Externalization

## Problem
Heavy agent tools like `mcp_git.c` (51KB) are compiled into the core `aimee` binary. This increases the complexity of the main executable and forces all users to carry the weight of specialized tools they may not need.

## Goals
- Decouple heavy tools into standalone MCP-compatible binaries.
- Allow for a truly modular tool ecosystem.

## Approach
Extract the logic from `mcp_git.c` into a standalone `aimee-mcp-git` binary. The core `aimee` client will act as a thin proxy, routing tool requests to these external binaries via stdio or Unix sockets. This follows the Model Context Protocol design for extensibility.

## Acceptance Criteria
- [ ] `mcp_git.c` removed from core library; moved to standalone executable.
- [ ] Agents continue to function using `git_status`, `git_commit`, etc., via the proxy.
- [ ] Core `aimee` binary size reduced by ~50KB.
