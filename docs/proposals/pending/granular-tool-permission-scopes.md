# Proposal: Granular Tool Permission Scopes

## Problem

Aimee's guardrails (`guardrails.c`) classify paths as sensitive and block known-dangerous operations, but permissions are binary: a tool is either enabled or disabled, and path checks are hardcoded patterns. There is no way to express fine-grained policies like:

- "This agent can only write files under `src/`"
- "Shell commands matching `rm -rf *` are blocked, but `rm` with specific paths is allowed"
- "File reads outside the project directory require approval"
- "Web fetch is restricted to internal URLs only"

This forces a choice between overly permissive (auto-approve everything) and overly restrictive (require approval for every tool call). Neither is good for autonomous workflows.

Inspired by mistral-vibe's `PermissionScope` system, which defines per-tool permission scopes for command patterns, file patterns, URL patterns, and directory boundaries.

## Goals

- Tool permissions can be scoped by file pattern, command pattern, URL pattern, and directory.
- Scoped permissions are configurable per agent profile and per project.
- Permissions are enforced in the pre-tool-check path (guardrails), not in individual tools.
- The system works identically in CLI and webchat execution paths.
- Unapproved scopes prompt the user (CLI) or are blocked with explanation (webchat).

## Approach

Extend the guardrails system with a permission scope model:

```c
typedef enum {
    SCOPE_FILE_PATTERN,       /* glob on file paths */
    SCOPE_COMMAND_PATTERN,    /* regex on shell commands */
    SCOPE_DIRECTORY,          /* restrict to directory tree */
    SCOPE_URL_PATTERN         /* glob/regex on URLs */
} permission_scope_t;

typedef struct {
    char tool_name[64];
    permission_scope_t scope;
    char pattern[256];        /* the allow/deny pattern */
    int allow;                /* 1 = allow, 0 = deny */
} permission_rule_t;
```

Rules are loaded from config (per-project `.aimee/permissions.toml` or global config). During `pre_tool_check`, the guardrails system extracts the relevant scope data from tool arguments (file path, command string, URL) and evaluates it against the rule set. First matching deny rule blocks; first matching allow rule permits; no match falls through to the default policy (prompt or block).

### Rule Evaluation Order

1. Explicit deny rules (highest priority)
2. Explicit allow rules
3. Default policy (configurable: `prompt`, `allow`, `deny`)

### Example Configuration

```toml
# .aimee/permissions.toml
default_policy = "prompt"

[[rules]]
tool = "bash"
scope = "command_pattern"
pattern = "rm -rf /"
allow = false

[[rules]]
tool = "write_file"
scope = "directory"
pattern = "src/"
allow = true

[[rules]]
tool = "write_file"
scope = "file_pattern"
pattern = "*.env*"
allow = false

[[rules]]
tool = "web_fetch"
scope = "url_pattern"
pattern = "https://internal.*"
allow = true
```

### Changes

| File | Change |
|------|--------|
| `src/headers/guardrails.h` | Add permission scope types, rule struct, evaluation API |
| `src/guardrails.c` | Add rule loading, scope extraction, evaluation logic |
| `src/config.c` | Parse `permissions.toml` from project and global config |
| `src/agent_policy.c` | Wire permission evaluation into `pre_tool_check` |
| `src/webchat.c` | Return permission denial reasons in tool-call error responses |
| `src/webchat_assets.c` | Show permission denial UI feedback in webchat |

## Acceptance Criteria

- [ ] File-pattern scope blocks writes to `*.env*` files when configured
- [ ] Directory scope restricts writes to the configured subtree
- [ ] Command-pattern scope blocks matching shell commands
- [ ] URL-pattern scope restricts web fetch to allowed domains
- [ ] Deny rules override allow rules
- [ ] Default policy (prompt/allow/deny) applies when no rule matches
- [ ] Rules load from both project-local and global config
- [ ] `aimee rules permissions` lists active permission rules
- [ ] Webchat shows permission denial reason when a tool call is blocked
- [ ] Permission evaluation adds <1ms overhead per tool call

## Owner and Effort

- **Owner:** aimee
- **Effort:** M (2-3 days)
- **Dependencies:** None (builds on existing guardrails infrastructure)

## Rollout and Rollback

- **Rollout:** Opt-in. Without a `permissions.toml`, behavior is identical to today.
- **Rollback:** Remove config file. No persistent state changes.
- **Blast radius:** Only sessions with permission config. Misconfigured rules could over-block — mitigated by `aimee rules permissions` listing and the `prompt` default policy.

## Test Plan

- [ ] Unit tests: rule matching for each scope type (file, command, URL, directory)
- [ ] Unit tests: deny-before-allow precedence, default policy fallthrough
- [ ] Integration tests: agent session with permission config, verify blocked/allowed tool calls
- [ ] Failure injection: malformed config file — must fail gracefully with warning, not crash
- [ ] Manual verification: configure restrictive rules, run an agent, observe correct blocking

## Operational Impact

- **Metrics:** `permission_denied{tool=...,scope=...}` counter, `permission_allowed` counter
- **Logging:** INFO on permission denial with rule details, DEBUG on evaluation
- **Alerts:** None
- **Disk/CPU/Memory:** Negligible. Rule set is small and evaluation is pattern matching.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Granular Tool Permission Scopes | P2 | M | High — enables safe autonomous operation with fine-grained control |

## Trade-offs

**Alternative: Extend existing guardrails patterns.** Adding more hardcoded patterns to `guardrails.c` doesn't scale and isn't user-configurable.

**Alternative: OPA/Rego policy engine.** Too heavy a dependency for the expected rule complexity. Simple glob/regex matching is sufficient.

**Known limitation:** Regex-based command matching can be bypassed by creative shell escaping. This is defense-in-depth, not a security boundary — the existing guardrails remain as a backstop.
