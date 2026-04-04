# Proposal: Plugin System for Aimee

## Problem

Aimee's tool surface and hook behavior are currently hardcoded. Adding new tools, hooks, or agent behaviors requires modifying aimee's C source directly. This creates several problems:

1. Per-project customizations (custom tools, domain-specific hooks) require forking or recompiling aimee.
2. Hook scripts exist (`configure-hooks.sh`) but have no lifecycle management — no install, enable/disable, update, or conflict detection.
3. There's no way for a project to ship its own agent tools alongside its codebase.

The `soongenwong/claudecode` repo at `rust/crates/plugins/` implements a complete plugin system:
- **Plugin manifest** (`.claw-plugin/plugin.json`): declares name, version, permissions, hooks, tools, and lifecycle commands
- **Plugin manager**: install from local path, enable/disable, list, uninstall — persisted in a registry JSON file
- **Plugin registry**: aggregates hooks and tools from all enabled plugins, detects name conflicts with builtins
- **Plugin hooks**: PreToolUse/PostToolUse hooks from plugins are merged with config-level hooks and executed in order
- **Plugin tools**: Plugins can declare custom tools with JSON schema, backed by executable scripts
- **Plugin lifecycle**: Init/Shutdown commands run when plugins are loaded/unloaded
- **Bundled plugins**: Ship default plugins in the binary for baseline behaviors

This is directly applicable to aimee's architecture.

## Goals

- Projects can ship `.aimee-plugin/` directories that declare custom hooks, tools, and lifecycle commands.
- Users can install, enable, disable, and remove plugins via `aimee plugin` commands.
- Plugin-provided hooks merge cleanly with aimee's existing hook system.
- Plugin-provided tools appear in MCP tool listings and are callable by all agents.
- Builtin/bundled plugins provide default behaviors that can be overridden.

## Approach

### Plugin Manifest Format

```
project-root/
  .aimee-plugin/
    plugin.json
  hooks/
    pre.sh
    post.sh
  tools/
    my-tool.sh
```

`plugin.json`:
```json
{
  "name": "my-project-plugin",
  "version": "1.0.0",
  "description": "Project-specific agent tools",
  "permissions": ["read", "execute"],
  "defaultEnabled": true,
  "hooks": {
    "PreToolUse": ["./hooks/pre.sh"],
    "PostToolUse": ["./hooks/post.sh"]
  },
  "tools": [
    {
      "name": "run_project_tests",
      "description": "Run this project's test suite",
      "command": "./tools/my-tool.sh",
      "input_schema": {
        "type": "object",
        "properties": {
          "filter": { "type": "string", "description": "Test name filter" }
        }
      }
    }
  ],
  "lifecycle": {
    "Init": ["echo plugin loaded"],
    "Shutdown": []
  }
}
```

### Changes

| File | Change |
|------|--------|
| `src/plugin.c` (new) | Plugin manifest parsing, installation, enable/disable, registry persistence |
| `src/plugin_hooks.c` (new) | Hook aggregation from enabled plugins, merged with config hooks |
| `src/plugin_tools.c` (new) | Plugin tool execution (subprocess with JSON stdin/stdout protocol) |
| `src/cmd_plugin.c` (new) | `aimee plugin install/list/enable/disable/remove` CLI commands |
| `src/mcp_tools.c` | Register plugin-provided tools in MCP tool listing |
| `src/cmd_hooks.c` | Merge plugin hooks into existing PreToolUse/PostToolUse pipeline |
| `src/config.c` | Add plugin config paths, plugin enable/disable state |
| `src/headers/plugin.h` (new) | Public plugin API |

### Plugin Registry Storage

```
~/.config/aimee/plugins/
  installed.json     # [{name, version, source, enabled, kind}]
  settings.json      # per-plugin overrides
```

### Hook Execution Order

1. Config-level hooks (from `settings.json`)
2. Bundled plugin hooks (ship with aimee)
3. External plugin hooks (installed by user)
4. Project-local plugin hooks (from `.aimee-plugin/` in workspace)

### Tool Conflict Resolution

- Plugin tools cannot shadow builtin tool names (error on install).
- Two plugins cannot declare the same tool name (error on install of second).
- Plugin tools are namespaced in listings: `plugin:tool_name`.

### Tool Permission Levels

Plugin tools must declare a required permission level (from claw-code's `ToolSpec` pattern):

```json
{
  "name": "run_deploy",
  "permission": "dangerous",
  "command": "./tools/deploy.sh",
  ...
}
```

Valid levels: `read` (file inspection), `write` (file modification), `execute` (shell commands), `dangerous` (network, destructive ops). The session's permission policy determines whether the tool is auto-approved or prompted.

### Plugin Tool Execution Protocol

Plugin tools receive input via environment variables (matching claw-code's `PluginTool::execute()` pattern):
- `AIMEE_PLUGIN_ID`: Plugin identifier
- `AIMEE_PLUGIN_NAME`: Human-readable name
- `AIMEE_TOOL_NAME`: Tool being invoked
- `AIMEE_TOOL_INPUT`: JSON-encoded arguments on stdin

Tools write their result to stdout (JSON). Non-zero exit = error.

### Webchat Plugin Management

- **Webchat UI**: Add a "Plugins" panel accessible from session settings showing installed plugins with enable/disable toggles
- **API**: `GET /api/plugins` lists installed plugins; `POST /api/plugins/{id}/toggle` enables/disables
- **Dashboard**: Add a "Plugins" card showing installed count, hook execution stats, and any recent hook failures

## Acceptance Criteria

- [ ] `aimee plugin install ./path` installs a plugin from a local directory
- [ ] `aimee plugin list` shows installed plugins with name, version, enabled status, and kind
- [ ] `aimee plugin enable/disable <name>` toggles plugin without uninstalling
- [ ] `aimee plugin remove <name>` uninstalls a plugin
- [ ] Plugin hooks execute in correct order alongside config hooks
- [ ] Plugin tools appear in `aimee mcp` tool listings and are callable by agents
- [ ] Tool name conflicts with builtins are rejected at install time
- [ ] Plugin lifecycle Init commands run on first use, Shutdown on session end
- [ ] Plugin tools declare permission levels; session policy respects them
- [ ] Plugin tools receive input via env vars and stdin JSON, output via stdout
- [ ] **Webchat**: plugins panel shows installed plugins with toggle controls
- [ ] **Dashboard**: plugin card shows hook execution stats

## Owner and Effort

- **Owner:** delegate (code)
- **Effort:** L (4-6 days)
- **Dependencies:** None directly, but benefits from "improve-module-boundaries" proposal

## Rollout and Rollback

- **Rollout:** Additive — no plugins installed means zero behavior change. Plugin discovery is explicit (install command or `.aimee-plugin/` in workspace).
- **Rollback:** `aimee plugin remove` for individual plugins, or delete `~/.config/aimee/plugins/` to reset entirely.
- **Blast radius:** A broken plugin hook could deny tool calls. Mitigation: plugin hooks that crash or timeout are treated as warnings (allow execution to continue), matching the convention in the reference implementation.

## Test Plan

- [ ] Unit tests: manifest parsing, registry CRUD, hook aggregation, tool conflict detection
- [ ] Integration tests: install plugin from temp dir, verify hooks fire, verify tools callable
- [ ] Failure injection: plugin hook exits non-zero, plugin hook hangs (timeout), corrupt manifest
- [ ] Manual verification: create a `.aimee-plugin/` in a project, delegate a task, confirm plugin hooks and tools are available

## Operational Impact

- **Metrics:** `plugins_installed`, `plugin_hook_runs_total`, `plugin_tool_calls_total`
- **Logging:** Plugin install/remove at INFO, hook execution at DEBUG, hook failures at WARN
- **Alerts:** None
- **Disk/CPU/Memory:** Negligible — plugin metadata is small JSON files, hook scripts are short-lived subprocesses

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Plugin manifest + registry | P2 | M | High — enables entire system |
| Plugin hooks | P2 | M | High — most immediate use case |
| Plugin tools | P3 | M | Medium — less common but powerful |
| CLI commands | P3 | S | Medium — management UX |
| Bundled plugins | P3 | S | Low — can be added incrementally |

## Trade-offs

- **Why not just use hooks directly?** Hooks are single scripts without lifecycle management. Plugins bundle hooks + tools + metadata + permissions into a versionable, installable unit. A project can ship a `.aimee-plugin/` and every developer gets the same agent behavior.
- **Why not MCP servers as plugins?** MCP servers are heavier (persistent processes). Plugins are lightweight (scripts invoked on demand). Both can coexist — a plugin could declare an MCP server in its lifecycle Init.
- **Why local-only (no marketplace)?** Start simple. A marketplace/registry can be added later once the manifest format and install flow are proven.

## Source Reference

Implementation reference: `soongenwong/claudecode` at `rust/crates/plugins/` — full plugin manager with manifest parsing, install/enable/disable, registry persistence, hook aggregation, tool conflict detection, and lifecycle management.
