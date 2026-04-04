# Proposals

## Pending

### P0 — High Priority / Core Fixes

| Proposal | Effort | Summary | Dependencies |
|----------|--------|---------|--------------|
| [Work Queue Session Fix](proposals/pending/work-queue-session-fix.md) | S | Remove session ID constraints to allow seamless multi-terminal/agent use for a single user | None |
| [Migration ID Conflicts](proposals/pending/migration-number-conflicts.md) | S | Detect duplicate migration IDs early and provide a guided renumber path for agents | None |
| [Parallel Startup](proposals/pending/parallel-startup.md) | M | Parallelize session context assembly and worktree readiness checks | None |

### P1 — Workflow and Automation

| Proposal | Effort | Summary | Dependencies |
|----------|--------|---------|--------------|
| [Relocate Session State Out of the Repository](proposals/pending/relocate-session-state-out-of-repo.md) | M | Move transient worktrees and session artifacts out of the tracked repo tree | None |
| [Cascading Branch Merge](proposals/pending/cascading-branch-merge.md) | M | Orchestrate merging overlapping feature branches with auto-resolution patterns | None |
| [Delegate Git Operations](proposals/pending/delegate-git-operations.md) | M | Give delegates full git write capabilities in isolated worktrees | None |
| [Delegate File Output](proposals/pending/delegate-file-output.md) | S | Allow delegates to return specific file artifacts without full tool overhead | None |
| [Work Queue Audit Trail](proposals/pending/work-queue-audit-trail.md) | S-M | Log every state transition with timestamp, session, and local branch/commit results | Work Queue Session Fix |
| [Work Queue Claim Skip](proposals/pending/work-queue-claim-skip.md) | S | Support skipping items, filtering by tags/effort, and priority ordering in `work claim` | None |
| [Structural Budgets and Ownership Guards](proposals/pending/structural-budgets-and-ownership-guards.md) | S-M | Enforce file-size, hotspot ownership, and layer-boundary checks in CI | None |

### P2 — Enhancements and Tooling

| Proposal | Effort | Summary | Dependencies |
|----------|--------|---------|--------------|
| [Reduce Command Surface Area](proposals/pending/reduce-command-surface-area.md) | M | Separate core CLI workflows from advanced and experimental/admin commands | None |
| [Decompose Large Modules Roadmap](proposals/pending/decompose-large-modules-roadmap.md) | XL | Sequence hotspot refactors across command, service, and data layers | Improve Module Boundaries, Structural Budgets |
| [FTS5 Code Search](proposals/pending/fts5-code-search.md) | M | SQLite-based full-text index for faster, token-efficient code searching | None |
| [Code Call Graph](proposals/pending/code-call-graph.md) | M | Index symbol interactions (callers/callees) for improved blast radius analysis | None |
| [Token Usage Audit](proposals/pending/token-usage-audit.md) | S | Granular tracking of token costs per tool, project, and agent role | None |
| [Work Queue Idle GC](proposals/pending/work-queue-stale-claim-gc.md) | S | Reclaim abandoned claims based on idle age timeout | Work Queue Session Fix |
| [Delegate Discoverability](proposals/pending/delegate-help-and-discoverability.md) | S | Dynamic role listing and usage hints for `aimee delegate` | None |
| [Work Queue Source Paths](proposals/pending/work-queue-source-paths.md) | S | Use full paths in `source` field for unambiguous resolution of proposals | None |
| [Work Queue Naming](proposals/pending/work-queue-naming-clarity.md) | S | Rename `aimee queue` to `aimee logs` to avoid confusion with `aimee work` | None |
| [Work Queue Batch Dedup](proposals/pending/work-queue-add-batch-dedup.md) | S | Deduplicate `add-batch` against both pending and resolved (done/rejected) items | None |
| [Server-Side Context Cache](proposals/pending/server-side-context-caching.md) | M | Remote prompt caching for delegate and built-in chat providers | None |
| [Test Coverage Expansion](proposals/pending/test-coverage-expansion.md) | M | Fill remaining runtime gaps in direct tests for server and client components | None |

## Complete

| Proposal | Summary |
|----------|---------|
| [Non-Blocking Writes](proposals/done/nonblocking-server-writes.md) | EPOLLOUT-driven ring buffers for server writes |
| [Request Size Ceilings](proposals/done/request-size-ceilings.md) | Message size and JSON depth guards |
| [Capability Centralization](proposals/done/capability-policy-centralization.md) | Declarative method-to-capability policy table |
| [SQLite Reliability](proposals/done/sqlite-reliability-profile.md) | Pragma optimizations and status diagnostics |
| [Outcome Effectiveness](proposals/done/outcome-linked-effectiveness.md) | Success correlation for memory retrieval |
| [Delegation Robustness](proposals/done/delegation-robustness.md) | Retries, failure context, and progress checkpoints |
| [Structured Logging](proposals/done/structured-logging-audit.md) | Log levels and security audit trail |
| [MCP SSE/Resources](proposals/done/mcp-resources-prompts-sse.md) | Extended MCP support and SSE transport |
| [Multi-OS Portability](proposals/done/windows-portability.md) | macOS and Windows platform abstractions |
| [Kind Lifecycles](proposals/done/kind-specific-lifecycle.md) | Per-kind TTL and promotion rules |
| [Retrieval Planner](proposals/done/retrieval-planner.md) | Task-intent context allocation |
| [Write Quality Gates](proposals/done/memory-write-quality-gates.md) | Stability and conflict checks for memories |
| [Content Safety](proposals/done/content-safety-gates.md) | PII detection and retention enforcement |
| [Retrieval Quality](proposals/done/memory-retrieval-quality.md) | Offline eval harness and metrics |
| [Worktree Janitor](proposals/done/worktree-session-janitor.md) | Age-based stale worktree cleanup |
| [Performance SLOs](proposals/done/performance-slo-regression.md) | Latency and throughput benchmark suite |
| [Sanitizer/Fuzz CI](proposals/done/fuzzing-sanitizer-ci.md) | ASan/UBSan and libFuzzer integration |
| [State Export/Import](proposals/done/state-export-import.md) | Portable JSONL data migration |
| [Degraded Mode](proposals/done/offline-degraded-mode.md) | Actionable errors for unreachable providers |
| [Secret Keyring](proposals/done/secret-storage-hardening.md) | libsecret/Keychain integration |
| [Memory Linking](proposals/done/memory-to-memory-linking.md) | Cascade demotion and dependency hints |
| [Test Suite Cleanup](proposals/done/test-suite-signal-cleanup.md) | Replaced weak signal tests with deterministic assertions |
| [Adaptive Compaction](proposals/done/adaptive-compaction.md) | Quality-scaled term retention |
| [Async Git Fetch](proposals/done/async-git-fetch.md) | Background fetch during worktree creation |
| [Cache Base Branch](proposals/done/cache-base-branch.md) | Cached branch detection per workspace |
| [Migration Recovery](proposals/done/migration-recovery.md) | Corruption detection and backup recovery |
| [On-Demand Server](proposals/done/on-demand-server.md) | Auto-start/stop lifecycle |
| [Worktree Enforcement](proposals/done/worktree-enforcement.md) | Strict path-policy write blocking |
| [Binary Split](proposals/done/binary-split.md) | Client/Server architecture separation |
| [Launch Command](proposals/done/launch-command.md) | Session entry point with launch metadata |
| [Project Describe](proposals/done/project-describe.md) | Automated project context generation |
| [NAT Piercing](proposals/done/nat-piercing.md) | Reverse SSH tunnel for remote access |
