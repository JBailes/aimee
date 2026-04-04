# Proposal: Agent Infrastructure Context Awareness

## Problem

When asked to "execute infrastructure proposals," the agent failed repeatedly due to missing context that aimee should have provided. The user had to correct four distinct failures:

1. **Stale local state — didn't pull from main.** The infrastructure repo had 4 pending proposals on `origin/main` that weren't in the local checkout. Aimee's context provided no indication that the local repo was behind remote, so the agent explored empty directories and concluded nothing was pending. The user had to ask "have you pulled from main?"

2. **No awareness of pending proposals.** Aimee's key facts and memory contained no mention of infrastructure proposals, their lifecycle status, or that review findings existed with 30+ actionable items. The agent needed 6+ tool calls of manual directory exploration to discover the proposal system structure.

3. **Observability facts not actionable.** The `observability-stack` key fact states what's deployed (Loki, Prometheus, Grafana) but not how to use it — no Grafana URL, no dashboard names, no available metrics. The user had to remind the agent "we do have a grafana and observation server that you should use."

4. **No project-to-project boundary awareness.** When asked to write aimee proposals, the agent wrote them in the infrastructure project's `docs/proposals/pending/` directory instead of aimee's `docs/proposals/pending/`. Aimee's context doesn't describe which project owns which type of proposal. The user had to correct: "Aimee proposals should go in aimee, not infrastructure."

## Goals

- Agents working on infrastructure tasks know to pull from remote before inspecting local proposal state
- Agents know where pending proposals exist and their current count/status without manual exploration
- Agents know the Grafana URL, available dashboards, and key metrics so they can use observability tools as a first step
- Agents know which project owns which proposal domain (infrastructure proposals → infrastructure repo, aimee proposals → aimee repo)

## Approach

### 1. Git freshness guidance in context

Add a rule or key fact that instructs agents to `git pull` on any project repo before inspecting file-based state (proposals, configs, inventory). This is especially important for repos the agent doesn't directly work in (like infrastructure when the working directory is wol-ai).

### 2. Infrastructure proposal status in memory

Store a memory entry summarizing:
- Where proposals live (`/root/dev/infrastructure/docs/proposals/`)
- Current status breakdown (how many pending, done, rejected, deferred)
- That three review documents exist with outstanding findings
- Update this when proposals change state

### 3. Expanded observability key fact

Replace the current `observability-stack` fact with one that includes:
- Grafana URL: `http://192.168.1.100:3000`
- Dashboard names: Service Health, Host Utilization
- Key available metrics: `up`, `probe_success`, `pve_cpu_usage_ratio`, `pve_memory_usage_bytes`, `pg_up`
- Prometheus URL: `192.168.1.100:9090`
- Loki URL: `192.168.1.100:3100`

### 4. Project-proposal ownership mapping

Store a fact or rule that maps proposal domains to their owning projects:
- Infrastructure/homelab/media proposals → `/root/dev/infrastructure/docs/proposals/`
- Aimee tooling/agent proposals → `/root/dev/aimee/docs/proposals/`
- Each project's proposals describe changes to that project's codebase

### Changes

| File | Change |
|------|--------|
| Aimee rules/facts | Add git-freshness rule: pull before inspecting file state in non-CWD repos |
| Aimee memory `infra-proposal-status` | New entry tracking infrastructure proposal lifecycle |
| Aimee fact `observability-stack` | Expand with actionable URLs, dashboards, metrics |
| Aimee rules/facts | Add project-proposal ownership mapping |

## Acceptance Criteria

- [ ] Agent asked to "execute infrastructure proposals" pulls from main before inspecting pending directory
- [ ] Agent knows there are N pending infrastructure proposals without manual directory traversal
- [ ] Agent asked to check service health queries Grafana at `http://192.168.1.100:3000` as first step
- [ ] Agent asked to write an aimee proposal writes it in `/root/dev/aimee/docs/proposals/pending/`, not infrastructure

## Owner and Effort

- **Owner:** Aimee / Infra
- **Effort:** S
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Update aimee memory entries, key facts, and rules via `aimee memory store` and rule configuration
- **Rollback:** Delete or revert the memory/fact entries
- **Blast radius:** Only affects agent context assembly — no code changes, no service impact

## Test Plan

- [ ] Manual: start a new session, ask agent to check infrastructure proposals, verify it pulls and finds them
- [ ] Manual: ask agent to write an aimee proposal, verify correct project directory
- [ ] Manual: ask agent to check service health, verify it uses Grafana URL

## Operational Impact

- **Metrics:** None
- **Logging:** None
- **Alerts:** None
- **Disk/CPU/Memory:** Negligible (memory entries are small text)

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Git freshness rule | P1 | S | Prevents stale-state failures |
| Proposal status memory | P1 | S | Eliminates wasted exploration |
| Observability fact expansion | P2 | S | Makes obs tools discoverable |
| Project-proposal mapping | P1 | S | Prevents cross-project misplacement |

## Trade-offs

**Memory entries vs rules.** Some of these (git freshness, project ownership) are behavioral guidance better suited as rules. Others (proposal status, obs details) are factual and suit memory entries. Using both mechanisms is appropriate but adds maintenance surface.

**Staleness risk.** The proposal status memory will go stale as proposals are completed. This is acceptable because the cost of a stale count is low (agent pulls and sees the real state), while the cost of no context at all is high (agent doesn't know proposals exist).
