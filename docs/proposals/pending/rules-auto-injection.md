# Proposal: Rules / Convention Auto-Injection

## Problem

Projects often have coding conventions documented in various files (`.editorconfig`, linter configs, style guides, `CONTRIBUTING.md`). Delegates are unaware of these conventions unless explicitly told about them, leading to code that violates project standards and requires manual correction.

Evidence: oh-my-openagent implements a `rules-injector` hook (`src/hooks/rules-injector/`) that discovers rule files (`.clinerules`, `.cursorrules`, project convention files) near the file being edited and injects them into the tool output. It calculates "distance" from the edited file to each rule file and injects the closest ones.

## Goals

- Auto-discover project convention/rule files when delegates edit code
- Inject relevant rules into the tool output so delegates follow them
- Support multiple rule file formats: `.editorconfig`, `CONTRIBUTING.md`, custom rule files
- Distance-based relevance: closer rule files take priority

## Approach

When a delegate uses Edit or Write on a file, scan for known convention files in the directory hierarchy. Inject the content of the closest matching rule file into the tool output.

### Supported rule files (in discovery order)

1. `.aimee/rules.md` (project-specific aimee rules)
2. `CONTRIBUTING.md` (contribution guidelines)
3. `.editorconfig` (editor conventions)
4. Custom paths defined in `.aimee/project.yaml`

### Distance scoring

Rule files in the same directory as the edited file score highest. Each parent directory level reduces the score. Only inject the top-scoring rule file to minimize context usage.

### Changes

| File | Change |
|------|--------|
| `src/mcp_tools.c` | Add rule file discovery and injection on Edit/Write |
| `src/config.c` | Parse custom rule file paths from project.yaml |

## Acceptance Criteria

- [ ] Rule files in the same directory as the edited file are injected
- [ ] Parent directory rule files are discovered with lower priority
- [ ] Injection is capped at 1 rule file per edit to limit context cost
- [ ] Custom rule file paths from project.yaml are supported
- [ ] No rule files produces no injection
- [ ] Injection happens once per directory per session (cached)

## Owner and Effort

- **Owner:** aimee
- **Effort:** S–M (1–2 days)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Compile-time inclusion; active when rule files exist or configured
- **Rollback:** Remove discovery; edits proceed without rule injection
- **Blast radius:** Adds text to Edit/Write tool output; no behavioral change

## Test Plan

- [ ] Unit test: rule file in same directory is discovered
- [ ] Unit test: rule file in parent is discovered when child has none
- [ ] Unit test: custom path from config is discovered
- [ ] Unit test: missing rule files cause no injection
- [ ] Unit test: injection is cached per session

## Operational Impact

- **Metrics:** Rule injections per session
- **Logging:** Log at debug level with rule file path
- **Disk/CPU/Memory:** Directory stat walk, cached; negligible

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Rules Auto-Injection | P2 | S–M | Medium — enforces project conventions in delegate output |

## Trade-offs

Alternative: include all rules in the delegate system prompt. Wastes context on rules irrelevant to the files being edited. Per-file injection is more targeted.

Alternative: lint after the fact and fix violations. Reactive rather than preventive — the delegate already wasted tokens producing non-conforming code.

Inspiration: oh-my-openagent `src/hooks/rules-injector/`
