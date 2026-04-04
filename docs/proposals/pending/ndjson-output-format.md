# Proposal: NDJSON Output Format for Programmatic Consumption

## Problem

Aimee's CLI outputs human-readable text. There's no structured output format for scripting or piping to other tools. Users who want to integrate aimee into CI pipelines, monitoring, or custom toolchains must parse unstructured text.

The `soongenwong/claudecode` repo supports three output formats: `text`, `json`, and `ndjson` (newline-delimited JSON for streaming). The NDJSON mode emits one JSON object per event, making it parseable by `jq`, log aggregators, or custom consumers.

This applies to CLI output and to webchat's API surface for programmatic clients.

## Goals

- `aimee --output ndjson delegate code "task"` emits one JSON object per event (tool call, tool result, text delta, usage).
- Events are parseable by `jq` and standard NDJSON consumers.
- Webchat API returns structured JSON responses (already partially done; this ensures consistency).
- Human-readable output remains the default.

## Approach

### Event Format

Each line is a self-contained JSON object:

```jsonl
{"type":"text_delta","content":"Let me look at "}
{"type":"text_delta","content":"the source file."}
{"type":"tool_use","name":"read_file","input":{"file_path":"src/config.c"}}
{"type":"tool_result","name":"read_file","output":"...","is_error":false}
{"type":"usage","input_tokens":1234,"output_tokens":567}
{"type":"done","cost_usd":0.42}
```

### Changes

| File | Change |
|------|--------|
| `src/cli_main.c` | Add `--output` flag (text/json/ndjson), emit structured events in ndjson mode |
| `src/render.c` | Add `render_ndjson_event()` for each event type |
| `src/agent_eval.c` | Emit events through render layer (supports both text and ndjson) |
| `src/webchat.c` | Align webchat API response format with ndjson event schema |

## Acceptance Criteria

- [ ] `aimee --output ndjson delegate code "echo hello"` emits valid NDJSON
- [ ] Each event is on its own line and parseable by `jq`
- [ ] Events include: text_delta, tool_use, tool_result, usage, done
- [ ] Default output (no `--output` flag) is unchanged human-readable text
- [ ] `aimee --output json delegate code "task"` emits a single JSON object with the full result
- [ ] Webchat API responses align with the same event schema

## Owner and Effort

- **Owner:** delegate (code)
- **Effort:** S (1 day)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Opt-in via `--output` flag. Default behavior unchanged.
- **Rollback:** Remove `--output` flag.
- **Blast radius:** None — new flag, no existing behavior changed.

## Test Plan

- [ ] Unit tests: NDJSON event serialization for each type
- [ ] Integration tests: pipe output through `jq` and verify valid JSON per line
- [ ] Manual verification: `aimee --output ndjson delegate code "ls" | jq .type`

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| NDJSON output mode | P3 | S | Medium — CI/scripting integration |
| JSON single-object mode | P3 | S | Medium — programmatic consumption |
| Webchat API alignment | P3 | S | Low — consistency |

## Source Reference

Implementation reference: `soongenwong/claudecode` at `rust/crates/claw-cli/src/args.rs` and `rust/crates/claw-cli/src/app.rs`.
