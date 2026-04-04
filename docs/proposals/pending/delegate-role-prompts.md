# Proposal: Configurable Delegate Role Prompt Templates

## Problem

When aimee delegates a task, the delegate receives a generic prompt: the task description plus some context. The prompt does not vary by role — a `review` delegation and a `code` delegation get the same framing, differing only in the user-provided task text.

This means:
- Delegates lack role-specific constraints (e.g., a reviewer should be read-only; a coder should follow minimal-change discipline)
- Delegates lack structured output guidance (e.g., reviewers should produce severity-classified findings; drafters should produce concise artifacts)
- The quality of delegation depends entirely on how well the *primary agent* writes the task prompt, rather than having institutional knowledge baked into the role

oh-my-codex ships 33 specialized role prompts (analyst, architect, critic, verifier, performance-reviewer, dependency-expert, etc.), each with:
- A clear identity and mission statement
- Explicit scope boundaries ("you do NOT do X")
- Output format requirements (evidence-dense, file:line citations, structured findings)
- Anti-pattern guards (what to avoid)
- Escalation rules (when to hand off to another role)

These prompts transform generic LLM delegates into focused specialists. The same model produces dramatically better output when given role-specific framing.

Evidence:
- `agent_coord.c` builds delegate prompts from task text + context but has no per-role template
- `DELEGATES.md` lists 10 roles (code, review, explain, refactor, draft, execute, summarize, format, search, reason) but none have associated prompt templates
- Delegate quality complaints often stem from vague or unfocused output — a symptom of generic prompting

## Goals

- Each delegate role has a configurable prompt template that frames the delegate's identity, constraints, output format, and scope boundaries
- Templates are user-customizable (not hardcoded) — different projects may need different review criteria
- The prompt assembly pipeline injects the role template before the task description
- Default templates ship for all 10 existing roles, providing immediate quality improvement

## Approach

### 1. Role template storage

Store templates in `~/.config/aimee/role_templates/`:

```
~/.config/aimee/role_templates/
  review.md
  code.md
  reason.md
  draft.md
  execute.md
  refactor.md
  summarize.md
  format.md
  search.md
  explain.md
```

Each file is a markdown prompt template with `{{TASK}}` and `{{CONTEXT}}` placeholders:

```markdown
# Role: Code Reviewer

You are a code reviewer. Your mission is to identify issues in the provided code changes that could cause bugs, security vulnerabilities, or maintainability problems.

## Constraints
- You are READ-ONLY. Do not suggest rewrites unless asked.
- Every finding must cite a specific file and line number.
- Distinguish between blocking issues (must fix) and suggestions (nice to have).

## Output Format
For each finding, provide:
- **Severity**: critical | high | medium | low
- **Category**: security | correctness | performance | maintainability | style
- **Location**: file:line
- **Description**: what the issue is
- **Suggestion**: how to fix it (one sentence)

If no issues are found, state "No issues found" — do not invent problems.

## Anti-patterns to avoid
- Generic praise ("looks good overall") without evidence
- Nitpicking style when there are real issues
- Suggesting refactors unrelated to the change

## Task
{{TASK}}

## Context
{{CONTEXT}}
```

### 2. Prompt assembly

Modify the delegate prompt builder in `agent_coord.c`:

```c
char *build_delegate_prompt(app_ctx_t *ctx, const char *role, const char *task, const char *context);
```

1. Check if `~/.config/aimee/role_templates/{role}.md` exists
2. If yes, load the template and substitute `{{TASK}}` and `{{CONTEXT}}`
3. If no, fall back to the current generic prompt format
4. Return the assembled prompt

### 3. Default templates

Ship default templates for all 10 roles during `aimee setup`. Key templates:

| Role | Key framing | Output format |
|------|------------|---------------|
| `review` | Read-only, evidence-dense, severity-classified | Structured findings with file:line |
| `code` | Minimal-change discipline, no refactoring beyond scope | Code diff or file contents |
| `reason` | Hypothesis-driven, falsify own conclusions | Ranked analysis with evidence |
| `refactor` | Preserve behavior, test-first | Before/after with justification |
| `draft` | Concise, actionable, no filler | Structured artifact |
| `execute` | Step-by-step, verify each step | Execution log with outcomes |
| `summarize` | Compress without losing key facts | Bullet points, max N items |
| `explain` | Audience-appropriate, concrete examples | Explanation with code references |
| `search` | Exhaustive within scope, cite sources | Findings with file:line |
| `format` | Transform data, preserve content | Formatted output |

### 4. Project-level overrides

Templates can also live at `.aimee/role_templates/` within a project, overriding user-level defaults. This lets projects customize review criteria, coding standards, etc.

Resolution order:
1. `.aimee/role_templates/{role}.md` (project)
2. `~/.config/aimee/role_templates/{role}.md` (user)
3. Built-in default (generic prompt)

### 5. CLI management

```bash
aimee roles list                    # show all roles and template status
aimee roles show <role>             # print the effective template for a role
aimee roles edit <role>             # open the user-level template in $EDITOR
aimee roles reset <role>            # restore the default template
```

### Changes

| File | Change |
|------|--------|
| `src/agent_coord.c` | Modify prompt builder to load and substitute role templates |
| `src/cmd_core.c` | Add `roles` subcommand |
| `src/config.c` | Add template path resolution (project → user → default) |
| `setup.sh` | Install default templates during `aimee setup` |
| `role_templates/*.md` | New: 10 default role template files |
| `src/tests/test_role_templates.c` | Tests for template loading, substitution, fallback |

## Acceptance Criteria

- [ ] Delegate prompts include role-specific framing from templates
- [ ] Default templates ship for all 10 roles
- [ ] `{{TASK}}` and `{{CONTEXT}}` substitution works correctly
- [ ] Project-level templates override user-level templates
- [ ] `aimee roles list` shows all roles and which template is active
- [ ] Missing templates fall back to generic prompt (no breakage)
- [ ] `aimee roles reset <role>` restores defaults

## Owner and Effort

- **Owner:** aimee
- **Effort:** M (2-3 focused sessions for infrastructure + 1-2 for crafting quality default templates)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** `aimee setup` installs defaults. Existing delegate behavior unchanged if templates aren't present.
- **Rollback:** Revert commit. Prompt builder falls back to generic format.
- **Blast radius:** Low. Templates are additive to prompts. Worst case: a bad template degrades one role's output.

## Test Plan

- [ ] Unit tests: template loading from project/user/default paths
- [ ] Unit tests: `{{TASK}}` and `{{CONTEXT}}` substitution
- [ ] Unit tests: fallback when template file missing
- [ ] Integration tests: delegate with template produces role-appropriate output format
- [ ] Manual verification: delegate a review task, observe structured findings output

## Operational Impact

- **Metrics:** `delegate_template_used{role,source=project|user|default}`
- **Logging:** `aimee: delegate [review]: using template from ~/.config/aimee/role_templates/review.md`
- **Alerts:** None
- **Disk/CPU/Memory:** Template files are <2KB each. Loading adds <1ms per delegation.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Template loading + substitution | P1 | S | High — enables everything else |
| Default templates for review, code, reason | P1 | M | High — immediate quality improvement |
| Default templates for remaining 7 roles | P2 | S | Medium — completeness |
| Project-level overrides | P2 | S | Medium — per-project customization |
| CLI management commands | P3 | S | Low — convenience |

## Trade-offs

**Why files instead of DB-stored templates?**
Templates are prose documents that users edit by hand. Files are natural for this — users can use their preferred editor, version-control them, and diff changes. DB storage would add friction without benefit.

**Why not hardcode templates in C?**
Hardcoded templates can't be customized per project or user. File-based templates are editable without recompilation. The fallback to generic prompts ensures the system works even without template files.

**Why 10 roles instead of adding more specialized ones (architect, critic, security-reviewer)?**
Adding roles requires delegate reconfiguration. The existing 10 roles cover the needed perspectives — a `review` template can specify a security focus, and a `reason` template can specify architectural analysis. Role templates are cheaper to change than role definitions.
