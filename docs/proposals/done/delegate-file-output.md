# Proposal: Delegate Artifact Output and Context Files

## Problem

The original proposal assumed delegates had no file capabilities at all. That
is no longer true: delegates already support `--tools`, `--files`, and
`--context-dir`, and tool-enabled delegates can write files through
`write_file`.

The remaining gap is explicit, caller-controlled artifact handling:

- there is still no simple `--output <path>` path for single-file drafts
- `--files` is a comma-separated preload, not a repeatable `--context-file`
  interface
- using `--tools` just to write one output file is heavier and riskier than
  needed

For common tasks like drafting a proposal, generating a config, or exporting a
review summary, the caller still has to capture stdout and redirect it
manually.

## Goals

- Add a simple, explicit single-file output mode for delegates
- Add repeatable context-file loading with clearer ergonomics than `--files`
- Keep tool-enabled delegates for multi-step tasks, but avoid requiring tools
  for straightforward artifact generation

## Approach

### 1. Add `--output <path>`

When present, write the final delegate response to the specified path instead of
stdout. Create parent directories when safe. Print the written path on success.

### 2. Add repeatable `--context-file <path>`

`--files` and `--context-dir` stay supported, but add a repeatable
`--context-file` flag for the common case where the caller wants to preload a
small number of specific inputs in a stable order.

### 3. Keep multi-file output out of scope

The earlier `--output-dir` idea depends on heuristic parsing of fenced blocks.
That is useful only after the single-file path exists and has proven value. For
now, keep this proposal focused on the deterministic path.

## Changes

| File | Change |
|------|--------|
| `src/cmd_agent_trace.c` | Add `--output` and repeatable `--context-file` flag parsing |
| `src/cmd_agent_trace.c` | Write final delegate response to the requested path when `--output` is set |

## Acceptance Criteria

- [ ] `aimee delegate draft --output docs/x.md "Write X"` creates the file
- [ ] `aimee delegate draft --context-file template.md "Follow this template"` preloads file contents
- [ ] Multiple `--context-file` flags are applied in the order provided
- [ ] Existing stdout behavior remains unchanged when `--output` is omitted
- [ ] Existing `--files` and `--context-dir` behavior remains unchanged

## Owner and Effort

- **Owner:** aimee core
- **Effort:** S
- **Priority:** P2

## Test Plan

- [ ] Unit test: `--output` writes the response and creates parent directories
- [ ] Unit test: unwritable `--output` path returns a clear error
- [ ] Unit test: repeatable `--context-file` preserves ordering
- [ ] Regression test: plain `aimee delegate draft "prompt"` still writes to stdout

## Trade-offs

This proposal deliberately does not replace tool-enabled delegates. If the task
needs multi-file edits or iterative verification, `--tools` remains the right
path. The value here is a low-friction artifact workflow for the common
single-file case.
