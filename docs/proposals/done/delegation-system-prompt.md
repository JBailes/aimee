# Proposal: Add Delegation Instructions to Chat System Prompts

## Problem

The system prompts for both the CLI chat and webchat only mention basic tools (bash, read_file, write_file, etc.) and say nothing about aimee's delegate system. The AI does not know it can run `aimee delegate <role> "prompt"` to offload work to sub-agents, which means users who ask for delegation get no response or the AI tries to figure it out by trial and error.

## Goals

- Both CLI and webchat system prompts include delegation instructions.
- The AI knows the `aimee delegate` command syntax, flags (`--tools`, `--background`), and how to poll status.

## Approach

Append a `# Delegation` section to the system prompt in both `wc_build_system_prompt()` (webchat.c) and `build_system_prompt()` (cmd_chat.c) documenting:

- `aimee delegate <role> "prompt" [--tools] [--background]`
- `aimee delegate status <task_id>` for background polling
- When to use delegates (offloading expensive/independent work)

## Files changed

- `src/webchat.c` — extended `wc_build_system_prompt()`
- `src/cmd_chat.c` — extended `build_system_prompt()`
