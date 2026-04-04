# Proposal: Claude Code Plugin and MCP Integration

## Problem

aimee has a full Codex plugin (`client_integrations.c`) with marketplace registration, config.toml enablement, and skills, but Claude Code gets no equivalent integration. The only Claude Code MCP mechanism is `.mcp.json` written to project directories during `aimee init` and `aimee setup`, but this file is never created in session worktrees. Since Claude Code sessions always run in worktrees (per aimee's session isolation), the MCP server is never discovered.

Evidence:
- `ensure_mcp_json()` in `cmd_core.c:16` is called from `cmd_init()` and `cmd_setup()` only
- `cmd_session_start()` in `cmd_hooks.c` does NOT call `ensure_mcp_json()`
- The worktree at `/root/.config/aimee/worktrees/88bc66d1-*/aimee/` has no `.mcp.json`
- The main repo at `/root/aimee/.mcp.json` exists but is irrelevant when CWD is a worktree
- Claude Code's `~/.claude/settings.json` has hooks but no `mcpServers` section
- No Claude Code project settings exist for aimee worktree paths
- aimee MCP tools (`search_memory`, `find_symbol`, `delegate`, `preview_blast_radius`, etc.) are invisible to the primary agent in Claude Code sessions

## Goals

- Claude Code sessions have access to aimee's MCP tools (search_memory, find_symbol, delegate, preview_blast_radius, record_attempt, list_attempts, get_host, list_hosts, delegate_reply)
- The integration is automatic: no manual configuration needed per-session or per-worktree
- Claude Code gets first-class plugin treatment equivalent to Codex (custom commands, usage guidance)
- The solution works for both worktree-based sessions and direct repo sessions

## Approach

Two complementary changes:

### 1. Register aimee MCP globally in Claude Code settings

Instead of relying on per-directory `.mcp.json` files (which break with worktrees), register the MCP server in `~/.claude/settings.json` under `mcpServers`. This makes aimee available in every Claude Code session regardless of CWD.

Add to `ensure_client_integrations()` in `client_integrations.c`: detect if `~/.claude` exists, and if so, ensure `mcpServers.aimee` is present in `~/.claude/settings.json`.

### 2. Install Claude Code custom slash commands

Create `.claude/commands/` entries that expose common aimee workflows as slash commands for the primary agent. These are analogous to the Codex SKILL.md but in Claude Code's native format.

Commands to create:
- `/aimee-search` - search aimee memory for project facts
- `/aimee-delegate` - delegate a bounded sub-task
- `/aimee-blast-radius` - preview impact of multi-file edits

These are installed to `~/.claude/commands/` so they're available globally.

### 3. Ensure .mcp.json in worktrees as fallback

Also call `ensure_mcp_json()` during `session-start` for the worktree CWD. This provides a fallback if the global settings approach isn't sufficient and ensures `.mcp.json` is present if other MCP-capable clients use the worktree.

### Changes

| File | Change |
|------|--------|
| `src/client_integrations.c` | Add `ensure_claude_code_integration()` that writes `mcpServers` to `~/.claude/settings.json` and installs custom commands to `~/.claude/commands/` |
| `src/client_integrations.c` | Call new function from `ensure_client_integrations()` when `~/.claude` exists |
| `src/cmd_hooks.c` | Call `ensure_mcp_json(cwd)` during session-start (worktree fallback) |
| `plugins/aimee/.claude/commands/aimee-search.md` | Custom command: search aimee memory |
| `plugins/aimee/.claude/commands/aimee-delegate.md` | Custom command: delegate a task |
| `plugins/aimee/.claude/commands/aimee-blast-radius.md` | Custom command: preview blast radius |

## Acceptance Criteria

- [ ] After `aimee setup`, `~/.claude/settings.json` contains `mcpServers.aimee` pointing to `/usr/local/bin/aimee-mcp`
- [ ] After session-start in a worktree, `.mcp.json` exists in the worktree root
- [ ] Claude Code sessions show aimee MCP tools (verify with ToolSearch or by calling search_memory)
- [ ] Custom slash commands are installed in `~/.claude/commands/` and usable
- [ ] Existing Claude Code settings (hooks, permissions) are preserved when updating settings.json
- [ ] Codex plugin integration continues to work unchanged

## Owner and Effort

- **Owner:** aimee
- **Effort:** M (medium, mostly C string manipulation and JSON merging in client_integrations.c)
- **Dependencies:** None. aimee-mcp binary and Claude Code are already installed.

## Rollout and Rollback

- **Rollout:** Automatic on next `aimee setup` or `aimee init`. Session-start worktree fallback is immediate.
- **Rollback:** Remove `mcpServers` key from `~/.claude/settings.json`. Remove command files from `~/.claude/commands/`.
- **Blast radius:** Only affects new Claude Code sessions. Existing sessions unaffected. Worst case: MCP server fails to start, Claude Code ignores it gracefully.

## Test Plan

- [ ] Unit tests: verify `ensure_claude_code_integration()` creates correct JSON structure
- [ ] Integration tests: run `aimee setup` and verify settings.json has mcpServers
- [ ] Integration tests: run `aimee session-start` in a worktree, verify `.mcp.json` exists
- [ ] Manual verification: start a Claude Code session after setup, confirm `mcp__aimee__*` tools appear
- [ ] Failure injection: corrupt settings.json, verify graceful handling (preserve existing, skip update)

## Operational Impact

- **Metrics:** None new.
- **Logging:** Optional stderr log when MCP registration succeeds/fails.
- **Alerts:** None.
- **Disk/CPU/Memory:** Negligible. One small JSON write to settings.json.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Global MCP registration in settings.json | P1 | S | High: makes all aimee MCP tools available |
| Worktree .mcp.json fallback | P1 | S | Medium: belt-and-suspenders reliability |
| Custom slash commands | P2 | S | Medium: convenience, discoverability |

## Trade-offs

**Alternative: Only use .mcp.json per-worktree.** Rejected because worktrees are ephemeral and created after Claude Code starts. The `.mcp.json` would need to exist before Claude Code reads the project, which is a race condition. Global settings avoids this entirely.

**Alternative: Inject MCP tool descriptions via session-start hook output.** Rejected because hook output is informational context, not tool registration. Claude Code needs actual MCP protocol for tool calls.

**Alternative: Use Claude Code project-level settings (`~/.claude/projects/*/settings.json`).** Partially viable but would require creating settings for every possible worktree path, which is impractical given the UUID-based worktree naming.
