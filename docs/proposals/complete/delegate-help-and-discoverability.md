# Proposal: Delegate Help, Usage Hints, and Role Listing

## Problem

`aimee delegate` still fails closed on the UX path: zero args and malformed
invocations produce usage errors, but the command does not act like a
discoverable interface. The current proposal is still correct, but it should be
more explicit about dynamic role discovery and about the now-larger flag set.

## Goals

- `aimee delegate --help` prints a real usage block
- `aimee delegate` with no args prints help instead of a fatal usage line
- Roles are discoverable from current config, not a hardcoded stale list
- Error messages for bad role names or missing prompt include the usage hint and
  the available roles

## Approach

### 1. Add proper help output

Print usage, examples, and high-value flags:

- `--json`
- `--background`
- `--durable`
- `--tools`
- `--files`
- `--prompt-file`
- `--context-dir`

### 2. Add `--list-roles`

Load `agents.json`, enumerate configured roles, and print:

- role name
- which agents can satisfy it
- whether the route is tool-capable

### 3. Improve validation failures

If the role is unknown or the prompt is missing, print the same help footer and
point the user to `aimee delegate --list-roles`.

## Changes

| File | Change |
|------|--------|
| `src/cmd_agent_trace.c` | Add `--help`, zero-arg help, and `--list-roles` support |
| `src/cmd_agent_trace.c` | Improve missing-prompt and unknown-role errors |

## Acceptance Criteria

- [ ] `aimee delegate --help` prints a full usage block
- [ ] `aimee delegate` with no args exits cleanly after printing help
- [ ] `aimee delegate --list-roles` shows currently configured roles
- [ ] Unknown roles print a helpful error plus the dynamic role list

## Owner and Effort

- **Owner:** aimee core
- **Effort:** S
- **Priority:** P2
