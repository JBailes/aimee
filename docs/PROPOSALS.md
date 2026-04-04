# Proposals

This index reflects the current consolidated pending proposal set in [`docs/proposals/pending/`](proposals/pending/). The older priority tables had drifted badly after multiple merges; this file now groups proposals by subsystem rather than pretending stale priority numbers are still authoritative.

## Pending

### Agent Runtime and Core Loops

| Proposal | Summary |
|----------|---------|
| [Agent Loop Middleware](proposals/pending/agent-loop-middleware.md) | Make per-turn behaviors composable instead of hardcoding them in the agent loop |
| [Concurrent Tool Execution](proposals/pending/concurrent-tool-execution.md) | Execute independent tool calls from a single model response in parallel |
| [Configurable Iteration Limits](proposals/pending/configurable-iteration-limits.md) | Cap per-message agent loops to prevent runaway spend |
| [Consecutive Message Compaction](proposals/pending/consecutive-message-compaction.md) | Merge same-role adjacent messages before API calls |
| [Message History Repair](proposals/pending/message-history-repair.md) | Repair inconsistent message histories before sending them back to a model |
| [Incremental SSE Parser](proposals/pending/incremental-sse-parser.md) | Make streaming parsing robust to split frames and chunk boundaries |
| [Thinking Block Support](proposals/pending/thinking-block-support.md) | Preserve and render reasoning/thinking blocks from newer model APIs |

### Delegation, Routing, and Parallelism

| Proposal | Summary |
|----------|---------|
| [Multi-Provider Routing, Aliasing, and Fallbacks](proposals/pending/multi-provider-delegate-drivers.md) | Unify provider drivers, model aliases, routing policy, ecomode, and fallback chains |
| [Coordinated Parallel Execution](proposals/pending/coordinated-parallel-execution.md) | Let related delegates share task-state and coordinate within one larger job |
| [Parallel Execution Waves](proposals/pending/parallel-execution-waves.md) | Execute independent plan steps in waves instead of strictly sequential order |
| [Per-Model Concurrency Limits](proposals/pending/per-model-concurrency-limits.md) | Avoid rate-limit storms by capping simultaneous delegates per model |
| [Delegate Role Prompts](proposals/pending/delegate-role-prompts.md) | Make delegate prompts role-specific instead of generic |
| [Delegate Token Budget](proposals/pending/delegate-token-budget.md) | Cap delegate prompt/context bloat before work starts |
| [Delegation Error Recovery](proposals/pending/delegation-error-recovery.md) | Turn bad delegation invocations into actionable retries instead of dead ends |
| [Delegate Liveness Monitoring and Circuit Breakers](proposals/pending/unstable-agent-babysitter.md) | Detect stuck, silent, looping, or idle delegates and recover or abort cleanly |
| [Subagent Depth Spawn Limits](proposals/pending/subagent-depth-spawn-limits.md) | Prevent unbounded delegation trees and recursive subagent fan-out |
| [Autonomous Mode Skip Permissions](proposals/pending/autonomous-mode-skip-permissions.md) | Launch agent CLIs in unattended mode when aimee is the safety boundary |

### Planning, Review, and Acceptance

| Proposal | Summary |
|----------|---------|
| [Planning Preparation and Review Pipeline](proposals/pending/requirements-clarification.md) | Unify clarification, planning depth selection, plan validation, and plan review |
| [Verification, Review, and Completion Pipeline](proposals/pending/structured-code-review.md) | Unify delegate verification, evidence capture, structured review, and retry-until-accepted execution |
| [Autonomous Pipeline](proposals/pending/autonomous-pipeline.md) | Chain clarify → plan → review → execute → QA → validate into one resumable end-to-end pipeline |
| [Evidence-Driven Diagnosis](proposals/pending/evidence-driven-diagnosis.md) | Add a diagnosis-first mode for bugs and incidents instead of jumping straight to fixes |
| [Investigation Notes](proposals/pending/investigation-notes.md) | Persist findings, dead ends, and conclusions during debugging/research work |
| [TDD Enforcement](proposals/pending/tdd-enforcement.md) | Encourage or enforce test-first sequencing in agent workflows |

### Context, Rules, and Prompt Assembly

| Proposal | Summary |
|----------|---------|
| [Hierarchical Context and Rule Discovery](proposals/pending/hierarchical-instruction-discovery.md) | Unify ancestor rule discovery, per-directory context, and local convention injection |
| [Skill Context Injection](proposals/pending/skill-context-injection.md) | Allow domain-specific skill guidance to be activated per session |
| [Per-Tool Prompt Injection](proposals/pending/per-tool-prompt-injection.md) | Let tools carry their own prompt guidance instead of centralizing all usage hints |
| [Tiered System Prompts](proposals/pending/tiered-system-prompts.md) | Support minimal/standard/extended prompt profiles |
| [Project-Scoped Workflow Learning](proposals/pending/project-scoped-workflow-learning.md) | Learn and surface project-specific workflow rules from behavior and correction |
| [File Reference Resolution](proposals/pending/file-reference-resolution.md) | Pre-resolve referenced files into delegate context to avoid wasting startup turns |
| [LSP Context Enrichment](proposals/pending/lsp-context-enrichment.md) | Inject diagnostics, definitions, and references into code-editing workflows |

### Tooling Safety, Validation, and Output Control

| Proposal | Summary |
|----------|---------|
| [Robust Tool Call Validation and Recovery](proposals/pending/robust-tool-call-validation.md) | Unify argument normalization, JSON recovery, schema checks, and edit-error recovery |
| [File Operation Safety Guards](proposals/pending/hash-anchored-edits.md) | Unify read-before-write protection and stale-edit detection |
| [Structured Diff Output and Edit Preview](proposals/pending/structured-diff-output.md) | Unify structured file-operation diffs with optional interactive edit preview |
| [Tool Result Compaction and Dynamic Truncation](proposals/pending/tool-result-compaction.md) | Compact oversized tool output before it bloats context |
| [Bash Command Guard](proposals/pending/bash-command-guard.md) | Steer agents toward structured tools instead of shell one-liners when possible |
| [Permission Escalation and Scoped Tool Permissions](proposals/pending/permission-escalation-framework.md) | Unify permission tiers, scoped rules, escalation, and tool allowlists |
| [Sandboxed Tool Execution](proposals/pending/sandbox-tool-execution.md) | Add namespace-based runtime isolation for dangerous tool execution |

### Session Continuity, UX, and Operator Visibility

| Proposal | Summary |
|----------|---------|
| [Session Compaction](proposals/pending/session-compaction.md) | Compact long sessions while preserving task state and repairing tool history |
| [Session Resume, Handoff, and Persistent Plan Progress](proposals/pending/session-resume.md) | Unify transcript resume, explicit handoff export, and persisted plan state |
| [Real-Time Session Status and HUD](proposals/pending/session-hud.md) | Unify terminal HUD and live per-session stats |
| [Slash Commands](proposals/pending/slash-commands-cli.md) | Add in-session commands for chat and webchat, including higher-level workflow commands |
| [Streaming REPL Upgrade](proposals/pending/streaming-repl-upgrade.md) | Add real-time tool feedback and richer streaming UX to CLI/webchat |
| [Markdown Rendering](proposals/pending/markdown-rendering-cli.md) | Render markdown and syntax-highlighted code in CLI and webchat |
| [Vim Keybindings CLI](proposals/pending/vim-keybindings-cli.md) | Add a stronger line editor for CLI chat |
| [Persistent Input History](proposals/pending/persistent-input-history.md) | Preserve interactive prompt history across chat sessions |
| [Turn Summary Narration](proposals/pending/turn-summary-narration.md) | Add lightweight post-turn summaries of what the agent just did |
| [Notification Hooks](proposals/pending/notification-hooks.md) | Fire desktop/webhook/command notifications for background or long-running events |

### Web and Programmatic Surfaces

| Proposal | Summary |
|----------|---------|
| [Session-Based SSE API for Webchat](proposals/pending/webchat-sse-session-api.md) | Make webchat sessions explicit, queryable, and observable via persistent SSE |
| [Programmatic Headless Mode and Structured Output](proposals/pending/programmatic-headless-mode.md) | Add `aimee run` plus text/JSON/NDJSON outputs for CI and automation |
| [OAuth PKCE MCP Client](proposals/pending/oauth-pkce-mcp-client.md) | Add authenticated remote MCP client support for OAuth-protected servers |

### Reliability, Infra, and Operations

| Proposal | Summary |
|----------|---------|
| [API Retry Backoff](proposals/pending/api-retry-backoff.md) | Add overflow-safe retries for transient provider/API failures |
| [Enterprise Proxy Support](proposals/pending/enterprise-proxy-support.md) | Support proxies and custom CA bundles in enterprise environments |
| [Graceful Cancellation](proposals/pending/graceful-cancellation.md) | Add one cancellation surface for long-running jobs, loops, and pipelines |
| [MCP Session-Aware Git](proposals/pending/mcp-session-aware-git.md) | Make MCP git operations aware of session/worktree context |
| [Background Process Management](proposals/pending/background-process-management.md) | Manage long-running subprocesses inside sessions |
| [OTEL Trace Export](proposals/pending/otel-trace-export.md) | Export execution traces to external observability systems |
| [Self-Update Notifier](proposals/pending/self-update-notifier.md) | Surface local binary/version drift and changelog hints |

### Developer Tooling and Code Intelligence

| Proposal | Summary |
|----------|---------|
| [AST-Grep Integration](proposals/pending/ast-grep-integration.md) | Add structural code search |
| [Plugin System](proposals/pending/plugin-system.md) | Make tools/hooks extensible without editing core C |
| [Stack-Detecting Init](proposals/pending/stack-detecting-init.md) | Bootstrap new projects with detected defaults and conventions |
| [Token Usage, Cost, and Context Budget Tracking](proposals/pending/token-cost-tracking.md) | Track usage, cache hits, cost, and context pressure consistently |

### Codebase Architecture and Refactoring

| Proposal | Summary |
|----------|---------|
| [Reduce Command Surface Area](proposals/pending/reduce-command-surface-area.md) | Split core workflows from experimental/admin commands |
| [Refactor cmd_core.c](proposals/pending/refactor-cmd-core.md) | Break up the monolithic command core |
| [Improve Module Boundaries](proposals/pending/improve-module-boundaries.md) | Reduce god files and sharpen architectural boundaries |
| [Structural Budgets and Ownership Guards](proposals/pending/structural-budgets-and-ownership-guards.md) | Enforce file-size and ownership limits in CI |
| [Decompose Large Modules Roadmap](proposals/pending/decompose-large-modules-roadmap.md) | Sequence deeper hotspot refactors across the codebase |
| [Refactor C Safety and Quality](proposals/pending/refactor-c-safety-and-quality.md) | Address broad C implementation risks and quality problems |
| [Dynamic String Handling](proposals/pending/dynamic-string-handling.md) | Replace dangerous fixed-size string patterns |
| [Extract Shared Utilities](proposals/pending/dry-extract-shared-utilities.md) | Remove duplicated helper logic |
| [Relocate Session State Out of Repo](proposals/pending/relocate-session-state-out-of-repo.md) | Move transient local state out of tracked repository paths |

### Behavior and Quality Guardrails

| Proposal | Summary |
|----------|---------|
| [AI Slop Detection and Cleanup](proposals/pending/ai-slop-detection.md) | Detect and optionally clean common low-quality AI-generated code patterns |
| [Orchestrator Self-Discipline](proposals/pending/orchestrator-self-discipline.md) | Keep the orchestrator coordinating instead of doing duplicate direct work |

## Notes

- This index intentionally does not attempt a strict total ordering. After consolidation, most priorities are easier to reason about within each subsystem than across the entire repo.
- A useful next step would be adding front-matter or a small metadata block (`priority`, `owner`, `dependencies`, `supersedes`) to every pending proposal so this index can be partially generated instead of hand-maintained.
