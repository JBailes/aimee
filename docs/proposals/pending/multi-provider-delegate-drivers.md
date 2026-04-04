# Proposal: Multi-Provider Routing, Aliasing, and Fallbacks

## Problem

The remaining provider/routing proposals are one stack:

- add a provider-driver abstraction
- support model aliases and provider autodetection
- add fallback chains
- make routing cost/complexity aware

These should not be separate proposals. Provider diversity only matters if routing, aliasing, and failover can actually use it.

## Goals

- A common driver interface abstracts provider differences.
- Users can refer to models by alias and let the system detect providers where possible.
- Routing can consider complexity, cost mode, and past success rates.
- Retryable failures can fall through configured fallback chains.
- Multiple provider profiles can coexist and participate in the same routing policy.

## Approach

Implement one provider/routing stack with four layers:

1. provider drivers and profile configuration
2. alias resolution and provider autodetection
3. cost/complexity-aware routing
4. fallback chains on retryable failures

### Driver Interface

Define a common delegate driver interface supporting:

- streaming chat
- tool-call parsing
- model listing
- capability metadata such as context limits and tool support

### Profiles, Aliases, and Detection

Support:

- named provider profiles
- short aliases such as `opus`, `sonnet`, `gpt4o`
- provider inference from model names
- OpenAI-compatible base URLs for local or third-party providers

### Routing

Before dispatch:

- classify task complexity
- apply normal or `ecomode` routing policy
- consider historical success rates per role/complexity
- choose the cheapest acceptable profile rather than always the globally cheapest one

### Fallback Chains

On retryable failures:

- consult role/profile fallback chains
- retry the same request on the next model/provider
- preserve session state across the switch

### XML Tool Fallback

For models without native tool calling, support XML-style tool-call parsing as a compatibility path.

### Changes

| File | Change |
|------|--------|
| `src/headers/delegate_driver.h` | Driver interface definition |
| `src/delegate_driver.c` | Driver registry and shared utilities |
| `src/delegate_openai.c` | OpenAI-compatible driver |
| `src/delegate_gemini.c` | Gemini driver |
| `src/delegate_xml_fallback.c` | XML tool-call parser |
| `src/model_registry.c` | Alias resolution, provider detection, model capability metadata |
| `src/agent_coord.c` | Complexity-aware routing, escalation, and fallback chains |
| `src/agent_config.c` | Load profiles from config |
| `src/config.c` | Parse provider profiles, fallback chains, and `ecomode` config |
| `src/cmd_core.c` | Add `ecomode` controls and routing stats |

## Acceptance Criteria

- [ ] The driver interface is implemented by at least 4 drivers: codex, ollama, openai, gemini.
- [ ] Model aliases like `opus` and `gpt4o` resolve to expected provider/model pairs.
- [ ] Provider auto-detection works for known model families and OpenAI-compatible endpoints.
- [ ] Delegates can be routed to any configured profile via `--profile`.
- [ ] Routing considers task complexity and `ecomode`.
- [ ] Retryable failures trigger configured fallback chains without losing session state.
- [ ] Existing codex and ollama behavior remains backward compatible.

## Owner and Effort

- **Owner:** delegate (code)
- **Effort:** L
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Ship driver abstraction and aliasing first, then routing logic, then fallback chains.
- **Rollback:** Existing codex/ollama behavior should remain the compatibility baseline.
- **Blast radius:** Medium. This touches the delegate dispatch path.

## Test Plan

- [ ] Unit tests: driver interface compliance, alias resolution, provider autodetection, routing heuristics, and fallback-chain decisions
- [ ] Integration tests: delegate task via OpenAI-compatible endpoint, verify tool call round-trip
- [ ] Failure injection: unreachable provider URL, retryable vs non-retryable failures
- [ ] Manual verification: configure a third-party profile and verify successful delegation

## Operational Impact

- **Metrics:** `delegate_calls_by_driver`, `delegate_driver_errors_total`, `delegation_escalations`, `fallback_triggers_total`, `ecomode_active`
- **Logging:** Alias resolution, routing decisions, fallback switches, driver init/failure
- **Alerts:** None initially
- **Disk/CPU/Memory:** Minimal beyond config and extra retry attempts

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Driver interface + registry | P1 | M | High |
| Alias resolution + autodetection | P1 | S | High |
| Complexity-aware routing + ecomode | P1 | M | High |
| Fallback chains | P1 | S | High |

## Trade-offs

- **Why merge the provider proposals?** Provider abstraction without routing policy is incomplete, and routing policy without provider abstraction has nothing to choose between.
- **Why include capability metadata?** Context limits and tool support affect routing, compaction, and fallback choices.
- **Why keep XML fallback?** Compatibility with weaker or older models is still useful for low-cost tiers.
