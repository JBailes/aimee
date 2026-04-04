# Proposal: Skill Context Injection

## Problem

When agents work in specific domains (debugging performance issues, writing database migrations, reviewing security code), they benefit from domain-specific guidance injected into their system prompt. Currently, aimee's system prompts are static — built once at session start. There's no way to dynamically inject domain-specific context mid-session without restarting.

ayder-cli implements a skill system:
- Skills are markdown files in `.ayder/skills/` that contain domain-specific guidance
- The `/skill <name>` command injects a skill's content into the system prompt mid-session as `### ACTIVE SKILL: <name>`
- Skills are auto-discovered and available for tab completion
- When a new skill is activated, the previous skill is replaced (only one active at a time)
- The TUI status bar shows the active skill name

This is directly applicable to aimee's architecture because:
- Delegates could be initialized with domain-specific skills based on the task
- The webchat interface could offer skill selection for different conversation modes
- Project-specific skills (shipped in the repo) would give agents project-aware guidance

Evidence:
- `agent_coord.c` builds delegate prompts with a fixed system prompt
- `webchat.c` builds system prompts once in `wc_build_system_prompt()`
- No mechanism to inject domain-specific guidance dynamically
- Different types of work (code review, debugging, migration writing) benefit from very different guidance

## Goals

- Projects can ship `.aimee/skills/` directories with domain-specific guidance as markdown files.
- Skills can be activated by name, injecting their content into the agent's system prompt.
- Only one skill is active at a time per session (prevents prompt bloat).
- Delegates can be initialized with a skill matching their task type.
- The webchat UI shows the active skill and allows switching.
- CLI commands support skill listing and activation.

## Approach

### 1. Skill storage

Skills are markdown files in `.aimee/skills/`:

```
.aimee/skills/
  security-review.md
  database-migration.md
  performance-debugging.md
  code-style.md
```

Each file is self-contained guidance:

```markdown
# Security Review

When reviewing code for security:
- Check all user input validation at system boundaries
- Verify authentication and authorization on every endpoint
- Look for SQL injection, XSS, command injection (OWASP Top 10)
- Check TLS configuration and certificate handling
- Verify secrets are not hardcoded or logged
- Check for timing attacks in authentication code
```

### 2. Skill discovery

```c
// Scan .aimee/skills/ and return available skill names
int skill_list(const char *project_root, char **names, int max_names);

// Load a skill's content by name
char *skill_load(const char *project_root, const char *name);
```

### 3. System prompt injection

When a skill is activated, append it to the system prompt:

```c
// Inject skill into system prompt (replaces any previous skill)
int skill_activate(session_t *session, const char *name);
```

The injection point is after the base system prompt and before the conversation history:

```
[base system prompt]
[tool descriptions]

### ACTIVE SKILL: security-review
[skill content]

[conversation messages...]
```

### 4. Auto-skill for delegates

In `agent_coord.c`, auto-select a skill based on the delegate role:

```c
const char *skill_for_role(const char *role) {
    // Map common roles to skills
    if (strcmp(role, "review") == 0) return "code-review";
    if (strcmp(role, "code") == 0)   return "code-style";
    // ... or check .aimee/skills/ for a file matching the role name
}
```

### 5. Webchat integration

Add a skill selector dropdown in the webchat UI. When changed, send a POST to `/api/skill` with the skill name. The backend injects the skill into the session's system prompt.

### 6. CLI commands

```
aimee skill list              # List available skills
aimee skill show <name>       # Print a skill's content
aimee skill activate <name>   # Activate for the current session (interactive mode)
```

### Changes

| File | Change |
|------|--------|
| `src/skill.c` (new) | Skill discovery, loading, activation, role-to-skill mapping |
| `src/headers/skill.h` (new) | Public skill API |
| `src/cmd_chat.c` | Support `/skill` command in interactive chat, inject into system prompt |
| `src/webchat.c` | Add `/api/skill` endpoint and skill selector, inject into session prompt |
| `src/webchat_assets.c` | Add skill dropdown to webchat UI |
| `src/agent_coord.c` | Auto-select skill based on delegate role |
| `src/cli_main.c` | Add `aimee skill` subcommands |

## Acceptance Criteria

- [ ] `.aimee/skills/` directory is scanned for available skills
- [ ] `aimee skill list` shows available skills
- [ ] Activating a skill injects its content into the system prompt
- [ ] Only one skill is active at a time (activating a new one replaces the old)
- [ ] Webchat UI shows a skill selector dropdown
- [ ] Delegates auto-select a skill matching their role when one exists
- [ ] Skills work as plain markdown files with no special syntax required

## Owner and Effort

- **Owner:** delegate (code)
- **Effort:** M (2-3 days)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Additive — no `.aimee/skills/` directory means no skills available. Skill activation is explicit.
- **Rollback:** Remove skill injection calls. System prompts revert to static.
- **Blast radius:** A very large skill file could bloat the system prompt. Mitigation: cap skill file size (e.g., 4KB) and warn on oversize.

## Test Plan

- [ ] Unit tests: skill discovery, loading, activation, role-to-skill mapping
- [ ] Integration tests: activate skill in CLI chat, verify system prompt includes skill content
- [ ] Integration tests: webchat skill selector, verify SSE responses reflect active skill
- [ ] Failure injection: missing `.aimee/skills/` directory, malformed skill file, very large skill file
- [ ] Manual verification: create a `security-review.md` skill, activate it, ask agent to review code, verify it follows the guidance

## Operational Impact

- **Metrics:** `skill_activations_total{skill="name"}`
- **Logging:** Skill activation at INFO, skill loading at DEBUG
- **Alerts:** None
- **Disk/CPU/Memory:** Negligible — skill files are small markdown, loaded once per activation

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Skill storage + discovery | P2 | S | High — enables everything |
| System prompt injection | P2 | S | High — core mechanism |
| Auto-skill for delegates | P2 | S | High — automatic domain awareness |
| Webchat selector | P3 | S | Medium — web UX |
| CLI commands | P3 | S | Low — management |

## Trade-offs

- **Why not just use CLAUDE.md?** CLAUDE.md is always-on static guidance. Skills are switchable domain-specific contexts. A security review skill would be noise during a performance debugging session. Skills let agents focus.
- **Why one skill at a time?** Multiple skills bloat the system prompt and can provide conflicting guidance. One skill keeps the prompt focused and the token budget predictable.
- **Why plain markdown?** No parsing overhead, human-readable, git-friendly. Developers can write and iterate on skills without learning a schema.

## Source Reference

Implementation reference: ayder-cli `tui/commands.py` — `/skill` command handler, `.ayder/skills/` discovery, and `### ACTIVE SKILL:` system prompt injection in `tui/app.py`.
