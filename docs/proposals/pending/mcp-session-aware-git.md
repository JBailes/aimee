# Proposal: Session-Aware MCP Git Tools

## Problem

MCP git tools are session-unaware. The standalone aimee-mcp binary and the server
both fail to find the correct worktree because session IDs don't match between
the hook subprocess and the MCP process.

## Solution

1. Pass session_id() from cli_mcp_serve proxy to server via mcp.call request
2. Server loads session state and chdirs to worktree for git tools
3. Update .mcp.json to use "aimee mcp-serve" instead of standalone aimee-mcp
4. Only two binaries: aimee (client) and aimee-server

## Status

Implemented in this commit.
