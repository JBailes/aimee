# Proposal: Thinking/Signature Block Support in Streaming

## Problem

Frontier models (Claude with extended thinking, future OpenAI reasoning models) emit `thinking` content blocks with optional cryptographic signatures. Aimee's streaming parser may not handle these block types, causing:

1. Thinking content is silently dropped or causes parse errors.
2. Signature verification is skipped — no way to detect if thinking blocks were tampered with.
3. Users in CLI and webchat can't see chain-of-thought reasoning even when the model produces it.

The `soongenwong/claudecode` repo at `rust/crates/api/src/types.rs` implements `Thinking`, `RedactedThinking`, `ThinkingDelta`, and `SignatureDelta` content block types with signature fields.

## Goals

- Aimee's streaming parser correctly handles `thinking` and `redacted_thinking` content blocks.
- Thinking content is displayed to users in both CLI (collapsible) and webchat (expandable panel).
- Signature fields are preserved and optionally verified.
- Thinking blocks are excluded from tool result context (they're for the user, not the next turn).

## Approach

### New Content Block Types

```c
typedef enum {
    BLOCK_TEXT,
    BLOCK_TOOL_USE,
    BLOCK_TOOL_RESULT,
    BLOCK_THINKING,           /* new */
    BLOCK_REDACTED_THINKING,  /* new */
} block_type_t;

typedef struct {
    char *thinking;    /* chain-of-thought text */
    char *signature;   /* cryptographic signature */
} thinking_block_t;
```

### Stream Event Handling

New delta types in the SSE stream:
- `content_block_start` with `type: "thinking"` → begin thinking block
- `thinking_delta` → append to thinking text
- `signature_delta` → append to signature
- `content_block_stop` → finalize thinking block

### Display

| Surface | Rendering |
|---------|-----------|
| CLI | Dimmed/italic text with "Thinking..." prefix, collapsed by default |
| Webchat | Expandable "Show thinking" panel with monospace rendering |

### Changes

| File | Change |
|------|--------|
| `src/agent_eval.c` | Parse `thinking` and `redacted_thinking` block types from stream |
| `src/server_session.c` | Store thinking blocks in session state (but exclude from tool context) |
| `src/render.c` | CLI rendering of thinking blocks (dimmed/italic) |
| `src/webchat.c` | Webchat expandable thinking panel |
| `src/headers/agent.h` | Add thinking block types to content block enum |

## Acceptance Criteria

- [ ] Streaming responses with `thinking` blocks are parsed without errors
- [ ] Thinking content is displayed in CLI and webchat
- [ ] `redacted_thinking` blocks show "[Thinking redacted]" placeholder
- [ ] Signature fields are preserved in session state
- [ ] Thinking blocks are NOT included in tool result context for the next turn

## Owner and Effort

- **Owner:** delegate (code)
- **Effort:** S (1 day)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Always-on — correctly parses new block types that were previously unhandled.
- **Rollback:** Revert — thinking blocks would be dropped again.
- **Blast radius:** None — additive parsing, no existing behavior changed.

## Test Plan

- [ ] Unit tests: parse thinking block from JSON, redacted thinking, signature field
- [ ] Integration tests: stream with thinking blocks from mock server
- [ ] Manual verification: enable extended thinking, verify display in CLI and webchat

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Stream parser support | P2 | S | High — future-proofs for reasoning models |
| CLI display | P3 | S | Medium |
| Webchat display | P3 | S | Medium |
| Signature verification | P3 | S | Low — defense in depth |

## Source Reference

Implementation reference: `soongenwong/claudecode` at `rust/crates/api/src/types.rs` (lines 138-204).
