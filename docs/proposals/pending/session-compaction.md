# Proposal: Session Compaction for Long-Running Agent Conversations

## Problem

When aimee delegates long-running tasks to agents, context windows fill up. Today, this is handled by the primary agent's own compaction (Claude Code's built-in `/compact`), but:

1. aimee's own working memory and session state has no compaction — it grows unboundedly within a session.
2. When aimee delegates to sub-agents (codex, ollama), those agents have smaller context windows and no built-in compaction at all.
3. Session resume (`aimee --resume`) loads the full conversation history, which can exceed context limits for long sessions.
4. There's no way to carry forward a structured summary of completed work when context gets large.

The `soongenwong/claudecode` repo at `rust/crates/runtime/src/compact.rs` implements a sophisticated compaction system:
- **Token estimation**: Fast `len/4 + 1` heuristic per content block
- **Threshold-based compaction**: Configurable `preserve_recent_messages` + `max_estimated_tokens`
- **Structured summaries**: Extracts scope, tools mentioned, recent user requests, pending work, key files referenced, current work, and key timeline
- **Iterative compaction**: When compacting an already-compacted session, merges "previously compacted context" with "newly compacted context" — no information is silently dropped
- **Summary formatting**: Strips analysis tags, extracts summary tags, collapses blank lines
- **Continuation messages**: Generates context for the model to resume seamlessly

This is directly applicable to aimee's delegate and session systems.

## Goals

- Long-running delegate sessions automatically compact when approaching context limits, preserving structured summaries of completed work.
- Session resume loads a compacted representation when the full history would exceed limits.
- Compaction preserves: recent messages verbatim, a structured summary of older messages, key file references, pending work items, and tool usage patterns.
- Iterative compaction merges prior summaries rather than discarding them.

## Approach

Implement compaction as a core library function in aimee, used by three consumers:

1. **Delegate sub-agents**: Before sending context to codex/ollama, compact if estimated tokens exceed the model's context window.
2. **Session persistence**: `aimee wm` (working memory) compacts older entries when the session grows large.
3. **Session resume**: When loading a session file, compact if it would exceed the target model's context limit.

### Token Estimation

Use the same heuristic as the reference: `strlen(content) / 4 + 1`. This is fast and close enough for compaction decisions (we don't need exact counts — we need to know when we're "getting large").

### Compaction Algorithm

```
1. Skip existing compacted-summary prefix message (if any)
2. Calculate compactable_messages = messages[prefix_len .. len - preserve_recent]
3. If compactable token estimate < max_tokens, do nothing
4. Generate structured summary of compactable_messages:
   - Message counts by role
   - Tool names mentioned
   - Recent user requests (last 3)
   - Pending work (messages containing "todo", "next", "pending", "remaining")
   - Key files (paths with known extensions extracted from content)
   - Current work (last non-empty text)
   - Key timeline (role + truncated content per message)
5. If prior compacted summary exists, merge:
   - "Previously compacted context:" + prior highlights
   - "Newly compacted context:" + new highlights
   - Combined timeline
6. Return: [compacted-summary-message] + [preserved recent messages]
```

### Changes

| File | Change |
|------|--------|
| `src/session_compact.c` (new) | Compaction algorithm: token estimation, summary generation, iterative merge |
| `src/headers/session_compact.h` (new) | Public API: `session_should_compact()`, `session_compact()`, `session_estimate_tokens()` |
| `src/agent.c` | Call `session_compact()` before sending delegate context when token estimate exceeds model limit |
| `src/working_memory.c` | Compact older working memory entries when session grows large |
| `src/server_session.c` | Compact on session resume when history exceeds configured limit |

### Summary Format

```
Summary:
- Scope: 42 earlier messages compacted (user=12, assistant=18, tool=12).
- Tools mentioned: Edit, Bash, Read, Grep.
- Recent user requests:
  - Fix the segfault in memory_promote when tier is NULL
  - Run the test suite after the fix
- Pending work:
  - Next: update tests for the new NULL guard
- Key files referenced: src/memory_promote.c, src/tests/test_memory.c
- Current work: Added NULL check in memory_promote_fact()
- Key timeline:
  - user: Fix the segfault in memory_promote when tier is NULL
  - assistant: tool_use Read({"file_path":"src/memory_promote.c"})
  - tool: tool_result Read: [file contents truncated]
  - assistant: Found the bug — tier can be NULL when...
  ...
```

### Continuation Message Template

When resuming from a compacted session, inject this system message (validated by claw-code's `compact.rs`):

```
This session is being continued from a previous conversation that ran out of context.
The summary below covers the earlier portion of the conversation.

[formatted summary]

Recent messages are preserved verbatim below this point.
Continue the conversation from where it left off without asking the user to repeat
anything. Do not re-summarize what was already done.
```

Include flags to suppress follow-up questions and note how many recent messages are preserved verbatim.

### Webchat and Dashboard Integration

Compaction events should be surfaced in both CLI and webchat:
- **CLI**: Print a one-line notice: `[compacted: 42 messages → summary, 4 recent preserved]`
- **Webchat**: Emit a `compaction` SSE event with message counts and summary; render as a collapsible system card in the chat UI
- **Dashboard**: Show compaction count per delegate in the delegations table

### Model-Aware Context Limits

Use model-specific max token limits when deciding whether to compact (from claw-code's `max_tokens_for_model()`):
- Opus models: 32,000 tokens
- Sonnet/Haiku models: 64,000 tokens
- OpenAI models: use model-specific limits from the API
- Compact when estimated tokens exceed 80% of the model's limit

## Acceptance Criteria

- [ ] `session_estimate_tokens()` returns a fast approximation within 2x of actual token count
- [ ] `session_should_compact()` correctly identifies sessions needing compaction based on configurable thresholds
- [ ] `session_compact()` produces a valid compacted session with structured summary + preserved recent messages
- [ ] Iterative compaction merges prior summaries (compacting an already-compacted session preserves "Previously compacted context")
- [ ] Delegates to codex/ollama automatically compact when context would exceed model limit
- [ ] Session resume with `--resume` compacts oversized session files
- [ ] Compacted summaries include: scope, tools, recent requests, pending work, key files, timeline
- [ ] Continuation message uses the standard template (no recap, no follow-up questions)
- [ ] **Webchat**: compaction events appear as collapsible cards in chat UI
- [ ] **Dashboard**: compaction count visible per delegation

## Owner and Effort

- **Owner:** delegate (code)
- **Effort:** M (2-3 days)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Compaction is automatic and transparent — agents see a summary + recent messages instead of the full history. Configurable via `compact_preserve_recent` (default 4) and `compact_max_tokens` (default 10000).
- **Rollback:** Set `compact_max_tokens` to a very large value to effectively disable. Or revert the code — session files are not modified in place (compaction produces a new representation).
- **Blast radius:** Compaction loses verbatim older messages. The structured summary preserves key information but not every detail. This is the intended trade-off — matching how Claude Code and other agents handle context limits.

## Test Plan

- [ ] Unit tests: token estimation accuracy, summary generation with various message shapes, iterative merge, edge cases (empty session, single message, all-tool messages)
- [ ] Integration tests: delegate a 50+ message task, verify compaction fires and delegate continues working
- [ ] Failure injection: corrupt summary prefix, messages with no text blocks, extremely long single messages
- [ ] Manual verification: run a long delegate session, inspect the compacted summary for quality

## Operational Impact

- **Metrics:** `session_compactions_total`, `session_messages_compacted`, `session_estimated_tokens`
- **Logging:** Compaction events at INFO (message count before/after, token estimate before/after)
- **Alerts:** None
- **Disk/CPU/Memory:** Compaction reduces memory usage. Summary generation is O(n) in message count with small constant factors.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Core compaction library | P1 | M | High — enables long-running delegates |
| Delegate integration | P1 | S | High — most common use case |
| Session resume integration | P2 | S | Medium — helps with session continuity |
| Working memory compaction | P3 | S | Low — WM is already relatively small |

## Trade-offs

- **Why not just use the model's built-in compaction?** Claude Code has `/compact`, but codex and ollama delegates don't. Even for Claude Code, aimee-level compaction means the delegate starts with a clean, structured context rather than relying on mid-conversation compaction.
- **Why not use an LLM to generate summaries?** The reference implementation uses heuristic extraction (not LLM-generated summaries) for speed and determinism. An LLM summary would be higher quality but adds latency and cost. The heuristic approach is good enough for context management.
- **Why preserve a timeline?** The timeline gives the model enough signal to understand the conversation flow without needing every message verbatim. This is validated by the reference implementation's test suite.

## Source Reference

Implementation reference: `soongenwong/claudecode` at `rust/crates/runtime/src/compact.rs` — 700-line compaction implementation with token estimation, structured summary generation, iterative merge, and comprehensive tests.
