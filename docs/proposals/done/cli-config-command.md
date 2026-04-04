# Proposal: CLI Config Command

## Problem

Changing the default AI CLI provider (claude, codex, gemini, copilot) requires manually editing `~/.config/aimee/config.json` or re-running `install.sh`. There is no `aimee config` command to read or write settings from the command line.

## Goals

- `aimee config set provider codex` sets the default CLI to Codex
- `aimee config get provider` shows the current provider
- `aimee config show` dumps all config as JSON
- Validates provider names and config keys

## Approach

Add `cmd_config` to `cmd_core.c` with three subcommands:

1. `aimee config show` - print current config as JSON
2. `aimee config get <key>` - print one config value
3. `aimee config set <key> <value>` - update one config value and save

Supported keys for get/set: `provider`, `use_builtin_cli`, `openai_endpoint`, `openai_model`, `openai_key_cmd`, `guardrail_mode`, `embedding_command`, `cross_verify`.

Provider validation: accept `claude`, `codex`, `gemini`, `copilot`, `openai`, or any non-empty string (forward-compatible with future CLIs).

### Changes

- `cmd_core.c`: add `cmd_config()` function
- `cmd_table.c`: add `{"config", "View and update configuration", cmd_config}` entry
- `headers/commands.h`: declare `cmd_config`

### Acceptance Criteria

- `aimee config show` outputs valid JSON matching config.json
- `aimee config get provider` prints current provider
- `aimee config set provider codex` updates config.json and subsequent `aimee` launches open Codex
- Unknown keys print an error
- Unit test covers get/set/show round-trip

### Owner

JBailes

### Rollback Plan

Remove the command table entry. No schema or data changes.

### Test Plan

- Unit test: set provider, reload config, verify value
- Manual: `aimee config set provider codex && aimee config get provider` prints "codex"

### Operational Impact

None. Read/write to existing config.json with existing atomic save.
