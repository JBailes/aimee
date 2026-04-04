# Proposal: Per-Tool Prompt Injection

## Problem

Aimee's system prompt is assembled centrally in `agent_context.c`. When a new tool is added or an existing tool has usage nuances, the system prompt must be manually updated to include guidance. This creates coupling: tool implementation and tool guidance are in different files, and it's easy to add a tool without updating the prompt (or to leave stale guidance after removing a tool).

Mistral-vibe implements per-tool prompts: each tool can specify a `prompt_path` or place a markdown file in a `prompts/` directory alongside the tool code. The system prompt assembler automatically discovers and injects these tool-specific prompts when the tool is enabled.

## Goals

- Each tool can contribute its own usage guidance to the system prompt.
- Tool prompts are automatically included when the tool is enabled and excluded when disabled.
- Tool prompt files live alongside the tool definition (co-location).
- Works for both built-in tools and MCP tools.
- Prompt assembly is consistent across CLI chat and webchat.

## Approach

### Prompt File Convention

For each tool registered in `tool_registry`, optionally store a prompt string in a new `tool_prompt` column. For built-in tools, load prompts from `src/tool_prompts/<tool_name>.md` at build time (embedded as string constants, same pattern as `webchat_assets.c`). For MCP tools, the prompt is provided in the tool's `description` field (already exists).

### System Prompt Assembly

In `agent_context.c` where the system prompt is built, after the base prompt sections, iterate over enabled tools and append their tool-specific prompts in a structured block:

```
## Tool Usage Notes

### bash
When running commands, prefer non-interactive flags...

### write_file
Always verify the target directory exists before writing...
```

### Changes

| File | Change |
|------|--------|
| `src/tool_prompts/` | New directory: per-tool markdown prompt files |
| `src/agent_context.c` | Append tool prompts to system prompt assembly |
| `src/agent_policy.c` | Add `tool_prompt` column to tool_registry, load from files |
| `src/db.c` | Add `tool_prompt` TEXT column to tool_registry table |
| `src/webchat.c` | Same prompt assembly path (already shares agent_context) |

## Acceptance Criteria

- [ ] Tool prompts are automatically included in the system prompt when the tool is enabled
- [ ] Disabling a tool removes its prompt from the system prompt
- [ ] Tool prompt files are co-located in `src/tool_prompts/`
- [ ] MCP tools use their description as the tool prompt
- [ ] System prompt assembly is identical for CLI and webchat
- [ ] Adding a new tool prompt requires only creating the file — no changes to prompt assembly code

## Owner and Effort

- **Owner:** aimee
- **Effort:** S (1 day)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Additive. No tool prompts exist initially — they're added incrementally.
- **Rollback:** Remove the tool_prompts directory. System prompt reverts to current behavior.
- **Blast radius:** System prompt content only. No functional changes.

## Test Plan

- [ ] Unit tests: prompt assembly with 0, 1, N tool prompts enabled
- [ ] Integration tests: disable a tool, verify its prompt is excluded
- [ ] Manual verification: inspect system prompt with `aimee agent context --show-prompt`

## Operational Impact

- **Metrics:** None
- **Logging:** DEBUG listing which tool prompts were included
- **Alerts:** None
- **Disk/CPU/Memory:** Negligible — a few KB of embedded strings

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Per-Tool Prompt Injection | P3 | S | Medium — improves tool guidance quality and maintainability |

## Trade-offs

**Alternative: Keep all guidance in one central prompt file.** Current approach. Doesn't scale as tool count grows and creates maintenance burden.

**Alternative: Include prompts only when the tool is first used in a session.** Saves tokens on initial prompt but adds complexity. Not worth it for the expected prompt sizes (~100 tokens per tool).

**Known limitation:** More tool prompts = larger system prompt = more input tokens. Mitigated by keeping prompts concise and only including enabled tools.
