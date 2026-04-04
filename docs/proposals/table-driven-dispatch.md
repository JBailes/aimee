# Proposal: Table-Driven Command Dispatch

## Problem
Aimee's current command dispatch is handled via fragmented `main` functions and nested `if/else if` blocks in `cmd_*.c`. This leads to redundant argument parsing, inconsistent help text, and high maintenance overhead when adding new subcommands.

## Goals
- Implement a centralized registry for all CLI commands.
- Eliminate > 1,000 lines of repetitive `getopt` and `argc` validation logic.
- Standardize subcommand routing and automatic help generation.

## Approach
We will implement a table-driven dispatch system in `cmd_registry.c`. Each command is defined as a static structure:
```c
typedef struct {
   const char *name;
   const char *help;
   int (*handler)(app_ctx_t *ctx, int argc, char **argv);
   const subcmd_t *subcommands;
} command_t;
```
A single `dispatch_command()` function will handle flag parsing for common options and route execution to the appropriate handler, automatically generating help output for unknown or missing arguments.

## Acceptance Criteria
- [ ] Command routing moved to a centralized table in `cmd_registry.c`.
- [ ] Individual `cmd_*.c` files reduced by ~20% through boilerplate removal.
- [ ] `aimee --help` correctly lists all commands and subcommands from the registry.
