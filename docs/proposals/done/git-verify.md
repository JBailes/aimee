# Proposal: `aimee git verify` - Pipeline Verification Gate

## Problem

No mechanism prevents pushing code that will fail CI/CD. Users can `aimee git push` without verifying build/test/lint pass locally.

## Solution

Add a `verify:` section to `.aimee/project.yaml` that defines commands to run before push is allowed. Track verification state via HEAD hash in `.aimee/.last-verify`. Gate `git_push` and `git_pr create` on verification.

## Config

```yaml
verify:
  - name: build
    run: cd src && make
  - name: unit-tests
    run: cd src && make unit-tests
  - name: lint
    run: cd src && make lint
```

## Interface

- `aimee git verify` - run all steps, record state
- `aimee git push` - blocked if not verified
- `aimee git push --skip-verify` - bypass
- `git_verify` MCP tool for AI agents

## New Files

- `src/git_verify.c` + `src/headers/git_verify.h`

## Modified Files

- `src/mcp_git.c` - push/PR create gate
- `src/cmd_core.c` - verify subcommand
- `src/mcp_server.c` - tool registration
- `src/headers/mcp_git.h` - declaration
- `src/Makefile` - build
