# Proposal: Autonomous Mode — Skip Agent Permission Prompts

## Problem

When aimee launches an agent CLI (Claude Code, Codex, Gemini CLI, Copilot), the agent runs in its default interactive permission mode. Every potentially risky tool call (file edits, shell commands, git operations) prompts the user for approval. This makes aimee unsuitable for unattended or batch workflows where aimee's own guardrails (`pre_tool_check`) are the intended safety boundary.

Each agent has its own mechanism to bypass interactive permissions:
- **Claude Code**: `claude --dangerously-skip-permissions`
- **Codex**: `codex --full-auto` (or `--auto-edit`)
- **Gemini CLI**: (flag TBD — may use `--non-interactive` or sandbox mode)
- **Copilot CLI**: (flag TBD)

Currently aimee launches the provider via bare `execlp(meta.provider, meta.provider, NULL)` with no arguments (`cli_main.c:408`). There is no way to pass these flags.

## Goals

- Configurable option to launch agent CLIs in autonomous mode (skip their permission prompts)
- Aimee's guardrails (`pre_tool_check`) remain the safety boundary — all tool calls still go through PreToolUse hooks
- Per-workspace or global configuration
- Clear opt-in: the user must explicitly enable this; never default to autonomous

## Approach

### 1. Add `autonomous` config flag

Add a boolean `autonomous` field to `config_t` (default: `0`). Settable via:
```bash
aimee config set autonomous true
```
Or in `~/.config/aimee/config.json`:
```json
{ "autonomous": true }
```

### 2. Map to provider-specific CLI flags

In `cli_main.c` where the provider CLI is launched (`execlp`), if `autonomous` is set, append the appropriate flag:

| Provider | Flag |
|----------|------|
| `claude` | `--dangerously-skip-permissions` |
| `codex` | `--full-auto` |
| `gemini` | TBD |
| `copilot` | TBD |

The mapping lives in a static table in `cli_main.c` or `client_integrations.c`.

### 3. Pass flag via launch metadata

The server's session-start handler builds launch metadata (`__LAUNCH__`). Add an `autonomous` field to the JSON. The CLI reads it and appends the flag when exec'ing the provider:

```c
/* cli_main.c — after building argv */
if (meta.autonomous) {
    const char *flag = autonomous_flag_for_provider(meta.provider);
    if (flag)
        execlp(meta.provider, meta.provider, flag, NULL);
    else
        execlp(meta.provider, meta.provider, NULL);
} else {
    execlp(meta.provider, meta.provider, NULL);
}
```

### 4. Guardrail enforcement remains active

The PreToolUse hook (`aimee hooks pre`) still runs for every tool call regardless of the agent's permission mode. Aimee's `pre_tool_check` is the safety net:
- Worktree enforcement (blocks writes to real repo when worktree is active)
- Sensitive file blocking (.env, credentials, keys)
- Git command redirection to MCP tools
- Path validation and blast-radius checks

The `guardrail_mode` config (`approve`/`prompt`/`deny`) continues to control aimee's own behavior independently of the agent's permission mode.

### Changes

| File | Change |
|------|--------|
| `src/headers/config.h` | Add `int autonomous;` to `config_t` |
| `src/config.c` | Load/save `autonomous` field, default `0` |
| `src/cli_main.c` | Read `meta.autonomous`, build provider argv with skip-permissions flag |
| `src/cli_launch.c` | Parse `autonomous` from launch metadata |
| `src/headers/cli_client.h` | Add `int autonomous;` to `launch_meta_t` |
| `src/cmd_hooks.c` (or server session-start) | Include `autonomous` in `__LAUNCH__` metadata |

## Acceptance Criteria

- [ ] `aimee config set autonomous true` enables the flag
- [ ] `aimee` (launch) starts Claude Code with `--dangerously-skip-permissions` when autonomous is set
- [ ] `aimee` launches without the flag when autonomous is not set (default)
- [ ] Guardrail hooks still fire on every tool call in autonomous mode
- [ ] Codex uses `--full-auto` when autonomous is set
- [ ] All existing tests pass

## Owner and Effort

- **Owner:** aimee core
- **Effort:** S
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Requires explicit opt-in via config. No behavior change for existing users.
- **Rollback:** `aimee config set autonomous false` or revert commit.
- **Blast radius:** Only affects how the provider CLI is launched. Aimee guardrails are unaffected.

## Test Plan

- [ ] Unit test: config load/save round-trips `autonomous` field
- [ ] Unit test: launch metadata includes `autonomous` flag
- [ ] Integration test: verify Claude Code receives `--dangerously-skip-permissions` when autonomous is set
- [ ] Manual: run `aimee` with autonomous enabled, verify no permission prompts from Claude Code, verify aimee guardrails still block sensitive operations

## Security Considerations

- Autonomous mode trusts aimee's guardrails as the sole safety boundary. If guardrails have bugs (e.g. the worktree enforcement bypass), the agent operates unrestricted.
- The config flag name should be clear about the implications. Consider naming it `autonomous` (not `skip_permissions`) to frame it as an aimee capability rather than a bypass.
- Log a warning on session-start when autonomous mode is active.

### Relationship to Permission Escalation Framework

The separate `permission-escalation-framework` proposal (inspired by claw-code's `runtime/permissions.rs`) defines a hierarchical permission model: ReadOnly < Write < Execute < Dangerous. Autonomous mode should integrate with it:

- `autonomous: false` + permission level `execute` → agent can read/write/execute but prompts for dangerous operations
- `autonomous: true` → sets permission level to `dangerous` (auto-approve all), but aimee guardrails still enforce blocklists
- The `--permission-mode` flag passed to Claude Code should map from aimee's permission level, not just be a binary on/off:
  - `PERM_READ` → `--permission-mode read-only`
  - `PERM_WRITE` → `--permission-mode workspace-write`
  - `PERM_DANGEROUS` / `autonomous` → `--permission-mode danger-full-access`

This gives finer control than just "autonomous on/off" — a user could allow workspace writes but prompt for bash execution.

### Webchat Integration

When launching agents via webchat, the permission level should be configurable per-session:
- **Webchat UI**: Add a "Permission Level" dropdown (read-only / workspace-write / full access) in the session settings panel
- **API**: `POST /api/sessions` accepts an optional `permission_level` field
- **Display**: Show current permission level as a badge in the webchat header

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Autonomous mode config + Claude Code flag | P1 | S | High — enables unattended workflows |
| Codex/Gemini/Copilot flag mapping | P2 | S | Medium — extends to other providers |
| Permission level mapping (from escalation framework) | P2 | S | High — granular control |
| Webchat permission selector | P3 | S | Medium — webchat UX improvement |
