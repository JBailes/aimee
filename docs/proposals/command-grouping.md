# Proposal: Command Category Grouping

## Problem
Aimee currently uses 18 separate `cmd_*.c` files. Each file duplicates the boilerplate for `arg_parse`, help text generation, and error reporting. This sprawl inflates the binary and makes the CLI difficult to extend consistently.

## Goals
- Group 18 fragmented command files into 4 cohesive category modules.
- Standardize error reporting and help text generation across all commands.

## Approach
Consolidate command handlers into the following categories:
1. **`cmd_agent.c`**: All agent execution, delegation, tracing, and chat commands.
2. **`cmd_memory.c`**: All memory, rules, working memory, and feedback commands.
3. **`cmd_infra.c`**: All workspace management, repository indexing, and infrastructure commands.
4. **`cmd_system.c`**: Core system commands: init, setup, version, mode, and hooks.

## Acceptance Criteria
- [ ] CLI source files reduced from 18 to 4.
- [ ] `aimee --help` output remains identical.
- [ ] Binary size reduction of ~40KB from removed boilerplate.
