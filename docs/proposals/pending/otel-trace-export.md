# Proposal: OpenTelemetry Trace Export

## Problem

Aimee captures detailed execution traces in `execution_trace` (SQLite) and mines them for patterns (`trace_analysis.c`), but this data is locked inside aimee's local database. The homelab already runs a full observability stack (Grafana at 192.168.1.100:3000, Prometheus at :9090, Loki at :3100), yet there is no way to visualize agent execution as distributed traces, correlate tool calls across delegates, or set up alerts on agent behavior.

Mistral-vibe implements OpenTelemetry integration that exports agent spans, tool call spans, and session metadata to any OTLP-compatible backend. This enables distributed tracing across the agent → delegate → tool call chain.

## Goals

- Agent executions emit OTLP spans to the observability stack.
- Each delegation creates a child span, making the full agent → delegate → tool call tree visible in Grafana Tempo or Jaeger.
- Token usage, cost, and error rates are exported as OTLP metrics alongside spans.
- Works for both CLI chat sessions and webchat agent executions.

## Approach

Add an optional OTLP exporter that hooks into aimee's existing execution trace system. Since aimee is C, we avoid the full OTEL SDK and instead emit OTLP/HTTP protobuf directly (the protocol is well-documented and compact).

### Span Model

```
Session Span (root)
  └─ Agent Turn Span
       ├─ Tool Call Span (bash)
       ├─ Tool Call Span (write_file)
       └─ Delegation Span
            └─ Delegate Turn Span
                 └─ Tool Call Span (...)
```

Each span carries attributes: `session.id`, `agent.name`, `agent.model`, `tool.name`, `tool.args_summary`, `tokens.prompt`, `tokens.completion`, `error` (if any).

### Integration Points

- **Agent loop (`agent.c`)**: Create session and turn spans around the main loop.
- **Tool execution (`agent_tools.c`)**: Create child spans for each tool call.
- **Delegation (`cmd_agent_delegate.c`)**: Propagate trace context (trace ID + parent span ID) to delegates.
- **Config (`config.c`)**: OTLP endpoint URL, enable/disable, export interval.
- **Webchat (`webchat.c`)**: Same span creation for webchat-initiated agent runs.

### Changes

| File | Change |
|------|--------|
| `src/headers/otel.h` | New: OTLP types, span API, exporter config |
| `src/otel.c` | New: span creation, OTLP/HTTP protobuf export, batch flushing |
| `src/agent.c` | Wrap agent loop turns in spans |
| `src/agent_tools.c` | Wrap tool executions in child spans |
| `src/cmd_agent_delegate.c` | Propagate trace context to delegates |
| `src/config.c` | Add otel config (endpoint, enable flag) |
| `src/webchat.c` | Same span instrumentation as CLI path |

## Acceptance Criteria

- [ ] `aimee config set otel.endpoint http://192.168.1.100:4318` enables export
- [ ] Agent session appears as a trace in Grafana Tempo / Jaeger
- [ ] Delegation chain is visible as nested spans
- [ ] Tool call spans show name, duration, and error status
- [ ] Token usage appears as span attributes
- [ ] Disabling otel (default) has zero overhead
- [ ] CLI and webchat produce identical span trees
- [ ] Batch export flushes at configurable interval (default 5s)

## Owner and Effort

- **Owner:** aimee
- **Effort:** L (3-5 days — protobuf encoding, span lifecycle, context propagation)
- **Dependencies:** OTLP-compatible collector on obs host (Grafana Alloy or otel-collector)

## Rollout and Rollback

- **Rollout:** Opt-in via config. Requires deploying an OTLP collector on the obs host.
- **Rollback:** Disable config flag. No persistent state changes.
- **Blast radius:** Only network traffic to the collector. Export failures are silently dropped (never block the agent loop).

## Test Plan

- [ ] Unit tests: span creation, protobuf encoding, batch flush
- [ ] Integration tests: agent session with OTLP mock server, verify trace structure
- [ ] Failure injection: collector unreachable — verify no impact on agent execution
- [ ] Manual verification: run a delegation chain, view trace tree in Grafana

## Operational Impact

- **Metrics:** OTLP metrics for token_usage, tool_call_duration, delegation_count
- **Logging:** WARN on export failures (rate-limited), DEBUG on span create/flush
- **Alerts:** Can now create Grafana alerts on agent error rates, delegation latency, etc.
- **Disk/CPU/Memory:** Negligible CPU for span creation. ~1KB per span in memory, flushed every 5s.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| OpenTelemetry Trace Export | P2 | L | High — unlocks full observability for agent workloads using existing infra |

## Trade-offs

**Alternative: Export to Prometheus only (metrics, no traces).** Simpler but loses the distributed trace tree which is the main value. Spans give per-call visibility; counters only give aggregates.

**Alternative: Use a C OTEL SDK.** None are mature enough for production. Direct OTLP/HTTP protobuf is simpler and has no dependencies.

**Known limitation:** Protobuf encoding adds ~200 lines of C. Could use JSON OTLP export instead (larger payloads but no protobuf dependency). JSON is acceptable for the expected volume.
