# Proposal: Quality Hardening (Completed Items)

Close-out for items completed from the original quality-hardening proposal.

## Performance Benchmarks — DONE (PR #94)

Implemented in `benchmarks/run.sh` and documented in `docs/BENCHMARKS.md`.
Covers startup, hook latency, memory search, memory list, agent network,
session-start, and memory maintenance with p50/p95/p99 percentiles.

## Security Threat Model — DONE (PR #94)

Implemented in `docs/SECURITY.md`. STRIDE-based threat model with trust
boundaries, attack surfaces table, and mitigations.

Note: The delegate system (`aimee delegate`) was not covered in the initial
threat model. It spawns sub-agents with SSH access to remote hosts and is
the highest-privilege attack surface. This should be added as a follow-up.
