# Proposal: Stack-Detecting Project Initialization

## Problem

Aimee has no guided project initialization. New users must manually write rules files and configure workspace settings. There's no auto-detection of the project's technology stack to seed sensible defaults.

This matters for CLI users running `aimee init` and for webchat users opening a new workspace — both should get a tailored starting configuration.

The `soongenwong/claudecode` repo at `rust/crates/claw-cli/src/init.rs` implements stack detection by probing for marker files and generating tailored guidance.

## Goals

- `aimee init` auto-detects the project's technology stack (C/Makefile, Rust/Cargo, Go/go.mod, Python/pyproject.toml, Node/package.json, etc.).
- Generates a starter `.aimee-rules` file with stack-specific verification commands and conventions.
- Idempotent — skips files that already exist.
- Works from CLI (`aimee init`) and webchat (auto-detect on workspace open, offer to generate).

## Approach

### Stack Detection Probes

| Probe File | Stack | Verification Commands |
|------------|-------|----------------------|
| `Makefile` + `*.c` | C | `make`, `make test` |
| `Cargo.toml` | Rust | `cargo fmt`, `cargo clippy`, `cargo test` |
| `go.mod` | Go | `go vet ./...`, `go test ./...` |
| `pyproject.toml` or `setup.py` | Python | `pytest`, `ruff check` |
| `package.json` + `tsconfig.json` | TypeScript | `npm run build`, `npm test` |
| `CMakeLists.txt` | CMake/C++ | `cmake --build .`, `ctest` |

### Generated .aimee-rules Template

```markdown
# Project: <directory name>

## Detected stack
- Languages: <detected>
- Build system: <detected>

## Verification
- Run: `<stack-specific commands>`

## Working agreement
- Prefer small, reviewable changes
- Update tests alongside code changes
```

### Changes

| File | Change |
|------|--------|
| `src/cmd_init.c` (new) | Stack detection probes, template generation, idempotent file creation |
| `src/cmd_core.c` | Register `aimee init` command |
| `src/webchat.c` | On new workspace open, auto-detect and offer to generate rules |

## Acceptance Criteria

- [ ] `aimee init` in a Rust project detects Cargo.toml and generates Rust-specific rules
- [ ] `aimee init` in a C project with Makefile detects C stack
- [ ] Running `aimee init` twice does not overwrite existing rules
- [ ] Webchat shows auto-detect banner for workspaces without rules
- [ ] Multiple stacks in one project are all detected

## Owner and Effort

- **Owner:** delegate (code)
- **Effort:** S (1 day)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Additive — new command, no existing behavior changed.
- **Rollback:** Remove command. Generated files deletable by user.
- **Blast radius:** None — user-initiated, idempotent.

## Test Plan

- [ ] Unit tests: stack detection with various marker file combos
- [ ] Integration tests: init in temp dirs with different stacks
- [ ] Manual verification: `aimee init` in real projects

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Stack detection + init | P3 | S | Medium — first-run experience |
| Webchat auto-detect banner | P3 | S | Medium — discoverability |

## Source Reference

Implementation reference: `soongenwong/claudecode` at `rust/crates/claw-cli/src/init.rs`.
