# Command Reference

All data-oriented commands support `--json` for machine-parseable output.

## Getting Started

These are the five commands most people use first:

- `aimee` — start a primary agent session with aimee hooks active. Example: `aimee`
- `aimee setup` — provision a workspace from the manifest. Example: `aimee setup`
- `aimee use <provider>` — switch the default provider quickly. Example: `aimee use claude`
- `aimee plan` — enable planning mode and block file writes. Example: `aimee plan`
- `aimee implement` — return to normal execution mode. Example: `aimee implement`

## Start and Configure aimee

Use these commands to launch aimee, prepare a workspace, and manage the active provider.

- `aimee` — launch a primary agent session with aimee hooks active. Example: `aimee`
- `aimee setup` — provision the workspace from the manifest. Example: `aimee setup`
- `aimee use <provider>` — set the default provider with the human-friendly shortcut form. Example: `aimee use codex`
- `aimee provider` — show the current default provider. Example: `aimee provider`
- `aimee provider <name>` — set the default provider. Example: `aimee provider gemini`
- `aimee plan` — switch to planning mode and block file writes. Example: `aimee plan`
- `aimee implement` — switch back to normal implementation mode. Example: `aimee implement`

## Fast Shortcut Forms

These shortcuts map to the existing scriptable commands. Keep using the old forms if you prefer; these simply make common actions faster to type.

- `aimee use claude` — expands to `aimee config set provider claude`. Example: `aimee use claude`
- `aimee provider` — expands to `aimee config get provider`. Example: `aimee provider`
- `aimee provider codex` — expands to `aimee config set provider codex`. Example: `aimee provider codex`
- `aimee verify on` — expands to `aimee verify enable`. Example: `aimee verify on`
- `aimee verify off` — expands to `aimee verify disable`. Example: `aimee verify off`

## Work With Memory

Use memory commands to search, store, version, and maintain persistent information.

- `aimee memory search <query>` — run a full-text search with FTS5. Example: `aimee memory search "provider defaults"`
- `aimee memory store <key> <value> --tier T --kind K` — store a memory entry. Example: `aimee memory store default_provider claude --tier working --kind fact`
- `aimee memory list [--tier T] [--kind K]` — list memories, optionally filtered by tier or kind. Example: `aimee memory list --tier working`
- `aimee memory supersede <id> <new>` — replace an existing fact with a new version. Example: `aimee memory supersede 42 "Use codex for tests"`
- `aimee memory stats` — show memory counts by tier and kind. Example: `aimee memory stats`
- `aimee memory maintain` — run the promotion and demotion maintenance cycle. Example: `aimee memory maintain`
- `aimee memory checkpoint create <name>` — create a named checkpoint. Example: `aimee memory checkpoint create pre-release`

## Track Tasks and Decisions

Use these commands when you want memory-backed task tracking or decision logs.

- `aimee memory task create <title>` — create a task. Example: `aimee memory task create "Refactor provider selection"`
- `aimee memory task list [--state S]` — list tasks, optionally filtered by state. Example: `aimee memory task list --state open`
- `aimee memory task update <id> <state>` — update a task state. Example: `aimee memory task update 12 done`
- `aimee memory task link <src> <tgt> <type>` — link two tasks with a relationship. Example: `aimee memory task link 12 15 blocks`
- `aimee memory decide <title> <choice> --options O` — record a decision and its options. Example: `aimee memory decide "Default provider" claude --options codex,claude,gemini`

## Give Feedback to the System

Use feedback commands to capture positive patterns and guardrails for the primary agent.

- `aimee + <text>` — record positive feedback; it becomes a rule for the primary agent. Example: `aimee + "Check merged PR state before pushing"`
- `aimee - <text>` — record negative feedback; it becomes a guardrail for the primary agent. Example: `aimee - "Do not edit directly on main"`

## Explore and Refresh the Code Index

Use the code index when you need symbol lookup, project overviews, impact analysis, or a fresh scan.

- `aimee index overview <project>` — show a project overview. Example: `aimee index overview aimee`
- `aimee index find <identifier>` — find a symbol or identifier. Example: `aimee index find SourceGeneratorWithAdditionalFiles`
- `aimee index blast-radius <project> <file>` — show dependents for a file. Example: `aimee index blast-radius aimee src/main.c`
- `aimee index scan <project> <root>` — re-scan a project from a root path. Example: `aimee index scan aimee /root/dev/aimee`

## Delegate Work to Other Agents

Use delegate agents for offloaded work, remote execution, or parallel help from configured sub-agents.

- `aimee agent list` — show configured delegate agents. Example: `aimee agent list`
- `aimee agent network` — show the network and host inventory available to delegate agents. Example: `aimee agent network`
- `aimee agent setup <provider>` — run interactive delegate agent setup. Example: `aimee agent setup claude`
- `aimee agent test <name>` — ping a delegate agent. Example: `aimee agent test build-bot`
- `aimee agent stats [name]` — show delegate call statistics, optionally for one agent. Example: `aimee agent stats` or `aimee agent stats build-bot`
- `aimee delegate <role> <prompt>` — route a task to the cheapest suitable delegate agent. Example: `aimee delegate reviewer "Check this diff for regressions"`

### Delegate Options

Use these flags with `aimee delegate` when you need more control over execution.

- `--tools` — enable tool-use mode with bash, `read_file`, and `write_file`. Example: `aimee delegate reviewer "Inspect this file" --tools`
- `--retry N` — automatically retry transient failures. Example: `aimee delegate implementer "Run the build" --retry 3`
- `--verify CMD` — run a verification command after delegation. Example: `aimee delegate fixer "Update docs" --verify "make test"`
- `--context-dir DIR` — bundle a directory into the prompt context. Example: `aimee delegate reviewer "Audit this module" --context-dir src`
- `--prompt-file PATH` — read the prompt from a file to avoid shell argument limits. Example: `aimee delegate analyst ignored --prompt-file /tmp/prompt.txt`
- `--files F` — pre-load file contents into the prompt. Example: `aimee delegate reviewer "Review these files" --files docs/COMMANDS.md,README.md`
- `--background` — fork the work and return a task ID immediately. Example: `aimee delegate implementer "Run a long migration audit" --background`

## Verify Work with Cross-Checking

Use cross-verification to have delegate agents review work automatically or on demand.

- `aimee verify enable` — turn on cross-verification. Example: `aimee verify enable`
- `aimee verify on` — alias for `aimee verify enable`. Example: `aimee verify on`
- `aimee verify config --verify-cmd CMD` — set the verification command. Example: `aimee verify config --verify-cmd "cd src && make unit-tests"`
- `aimee verify` — have a delegate agent review your current changes. Example: `aimee verify`
- `aimee verify disable` — turn off cross-verification. Example: `aimee verify disable`
- `aimee verify off` — alias for `aimee verify disable`. Example: `aimee verify off`

## Describe Projects

Use project description commands to generate or refresh project summaries.

- `aimee describe` — auto-describe all projects via delegate agents. Example: `aimee describe`
- `aimee describe <project>` — describe a specific project. Example: `aimee describe backend`
- `aimee describe --force` — re-describe even if up to date. Example: `aimee describe --force`

## Webchat

- `aimee webchat enable` — install and start the webchat systemd service. Example: `aimee webchat enable`
- `aimee webchat disable` — stop and disable the webchat service. Example: `aimee webchat disable`
- `aimee webchat status` — check if the webchat service is running. Example: `aimee webchat status`

## MCP Server

The MCP server runs as `aimee mcp-serve` (stdio JSON-RPC 2.0). It is built into the `aimee` client binary and auto-configured by `aimee setup`.

Tools exposed:

- `search_memory` — full-text search on stored facts
- `list_facts` — list all L2 facts
- `get_host` — look up a host by name
- `list_hosts` — list all hosts and networks
- `find_symbol` — search code index for symbol locations
- `delegate` — delegate a task to a sub-agent
- `preview_blast_radius` — show file dependency impact
- `record_attempt` — log a delegation attempt
- `list_attempts` — list recent delegation history
- `delegate_reply` — follow up on a prior delegation
