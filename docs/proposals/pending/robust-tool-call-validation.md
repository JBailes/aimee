# Proposal: Robust Tool Call Validation and Recovery

## Problem

Tool-call failures are currently fragmented across separate proposals:

- malformed or unknown tool calls
- argument aliasing and normalization
- malformed JSON arguments
- stale edit retries that should force a re-read

These are all one reliability surface: the tool layer should help the model recover from common mistakes instead of burning turns or aborting loops.

## Goals

- Normalize common argument mistakes before rejecting a call.
- Validate tool calls consistently before execution.
- Return actionable error tool results instead of aborting loops.
- Add focused recovery guidance for high-frequency edit failures.
- Work identically across CLI, webchat, and delegate sessions.

## Approach

Build one tool-call robustness pipeline with four stages:

1. argument normalization
2. JSON parsing and schema validation
3. graceful error tool results
4. post-error recovery guidance for edit failures

### Normalization

Handle common recoverable mistakes automatically:

- alias resolution such as `filepath` → `file_path`
- relative path resolution where safe
- simple type coercion such as `"5"` → `5`
- JSON cleanup for common malformed-but-fixable cases

### Validation

Before execution, validate:

- tool existence and enabled state
- argument parseability
- required fields
- basic type correctness
- unknown extra arguments

Unknown tools should include nearest-match suggestions.

### Error Tool Results

On validation failure, inject a synthetic tool result such as:

```json
{
  "tool_use_id": "<id>",
  "content": "Error: unknown tool 'read_flie'. Did you mean 'read_file'?",
  "is_error": true
}
```

### Edit Recovery

For common edit failures like:

- old string not found
- multiple matches
- identical old/new content

append recovery guidance that explicitly tells the agent to re-read before retrying.

### Changes

| File | Change |
|------|--------|
| `src/agent_policy.c` | Central validation and normalization pipeline |
| `src/agent.c` | Convert validation failures into error tool results |
| `src/mcp_tools.c` | Edit-error recovery directives |
| `src/cmd_chat.c` | Share the same validation/recovery behavior |
| `src/webchat.c` | Share the same validation/recovery behavior |

## Acceptance Criteria

- [ ] Unknown tool names return error results with nearest-match suggestions.
- [ ] Common argument aliases and simple type mismatches are normalized when safe.
- [ ] Malformed JSON and schema failures return actionable error tool results.
- [ ] Validation failures do not abort the overall agent loop.
- [ ] Edit failures append a concise “re-read before retrying” directive.
- [ ] CLI, webchat, and delegates share the same validation behavior.

## Owner and Effort

- **Owner:** aimee
- **Effort:** M
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Start with graceful failure and normalization, then add edit-specific recovery messaging.
- **Rollback:** Revert to strict validation without recovery hints.
- **Blast radius:** All tool paths; consistency matters more than feature breadth.

## Test Plan

- [ ] Unit tests: alias normalization, type coercion, malformed JSON handling
- [ ] Unit tests: unknown tool, disabled tool, missing field, wrong type, extra field
- [ ] Unit tests: edit-error recovery directives
- [ ] Integration tests: malformed tool calls self-correct after error results

## Operational Impact

- **Metrics:** `tool_validation_error`, `tool_argument_normalized`, `edit_recovery_triggered`
- **Logging:** WARN per validation failure, DEBUG on normalization/recovery actions
- **Alerts:** Warn if validation error rates spike
- **Disk/CPU/Memory:** Negligible

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Graceful validation failures | P1 | S | High |
| Argument normalization | P1 | S | High |
| Edit recovery directives | P1 | S | High |

## Trade-offs

- **Why merge these proposals?** Models do not care whether a tool failure came from bad JSON, a wrong field name, or a stale edit assumption; the recovery path should be unified.
- **Why normalize instead of always erroring?** Cheap recoverable fixes should not cost a whole turn.
- **Why still return explicit errors?** Silent coercion should stay conservative so the model can correct itself when guesses would be unsafe.
