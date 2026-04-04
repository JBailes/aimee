# Proposal: Robust Tool Call Validation with Graceful Failure

## Problem

When the LLM emits a malformed tool call — unknown tool name, invalid JSON arguments, missing required parameters, or wrong types — aimee's `tool_validate()` in `agent_policy.c` either silently passes (returns 0 on unknown tools) or sets a brief error string. The agent loop then fails in tool execution rather than catching the problem early and returning a useful error to the LLM so it can self-correct.

Mistral-vibe implements a three-layer pipeline: `ParsedToolCall` (raw extraction) → `ResolvedToolCall` (validated against schema) / `FailedToolCall` (captured with context). Failed calls are returned as tool results with descriptive error messages, letting the LLM retry with corrected arguments rather than crashing or hallucinating.

## Goals

- Invalid tool calls are caught before execution and returned as error tool results.
- The LLM receives actionable error messages ("unknown tool 'foo', did you mean 'bash'?", "parameter 'path' is required but missing").
- Tool call validation covers: unknown tools, disabled tools, missing required params, wrong param types, extra unknown params.
- Validation is consistent across CLI chat and webchat.

## Approach

### Validation Pipeline

Before executing any tool call, run a validation chain:

1. **Tool existence**: Is the tool name registered? If not, return error with closest match (Levenshtein distance).
2. **Tool enabled**: Is the tool currently enabled? If not, return "tool X is disabled."
3. **Argument parsing**: Does the JSON parse? If not, return "invalid JSON in arguments."
4. **Schema validation**: Do the arguments match the tool's `input_schema`? Check required fields, types, and enum values. Return specific field-level errors.

### Error Results

Instead of aborting the agent loop, inject a synthetic tool result:

```json
{
  "tool_use_id": "<id>",
  "content": "Error: unknown tool 'read_flie'. Did you mean 'read_file'?",
  "is_error": true
}
```

The LLM sees this as a tool result and can retry with the correct call.

### Changes

| File | Change |
|------|--------|
| `src/agent_policy.c` | Enhance `tool_validate()` with multi-stage validation, fuzzy name matching |
| `src/agent.c` | On validation failure, inject error tool result instead of aborting |
| `src/cmd_chat.c` | Same error injection for chat provider tool calls |
| `src/webchat.c` | Same error injection for webchat tool calls |

## Acceptance Criteria

- [ ] Unknown tool name returns error with closest match suggestion
- [ ] Disabled tool returns "tool X is disabled" error result
- [ ] Missing required parameter returns field-specific error
- [ ] Wrong parameter type returns type mismatch error
- [ ] Malformed JSON returns parse error
- [ ] All errors are returned as tool results (not loop aborts)
- [ ] LLM can self-correct after receiving validation errors
- [ ] Validation is identical in CLI and webchat paths
- [ ] Fuzzy name matching uses Levenshtein distance with threshold ≤ 3

## Owner and Effort

- **Owner:** aimee
- **Effort:** S (1-2 days)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Replaces existing validation. More permissive (errors become results, not aborts).
- **Rollback:** Revert to previous validation behavior.
- **Blast radius:** All tool call paths. Must handle both OpenAI and Anthropic tool call formats.

## Test Plan

- [ ] Unit tests: each validation stage with specific failure cases
- [ ] Unit tests: fuzzy name matching accuracy
- [ ] Integration tests: feed malformed tool calls, verify LLM self-corrects
- [ ] Manual verification: intentionally trigger each error type

## Operational Impact

- **Metrics:** `tool_validation_error{type=unknown|disabled|bad_json|schema}` counter
- **Logging:** WARN per validation error with details
- **Alerts:** WARN if validation error rate exceeds 20% (suggests prompt quality issue)
- **Disk/CPU/Memory:** Negligible

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Robust Tool Call Validation | P2 | S | High — reduces wasted turns from tool call errors |

## Trade-offs

**Alternative: Let the tool itself handle validation.** Some tools already do this, but inconsistently. Centralized validation ensures uniform error formatting and avoids duplicate validation code.

**Known limitation:** Schema validation is best-effort — aimee doesn't have a full JSON Schema validator in C. Check required fields and basic types; skip complex constraints like `oneOf`, `pattern`, etc.
