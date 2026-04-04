# Proposal: MCP Git Operations

## Problem

Every git operation a primary agent runs consumes tokens in both directions: the command itself and the output. Git output is notoriously verbose; `git diff`, `git log`, `git status`, and `git show` can easily produce thousands of tokens per invocation. Over a typical session with 15-30 git operations (status checks, staging, committing, pushing, PR creation, branch management, merge-status checks), this adds up to 20-50k tokens of context that the primary agent must process but that requires no creative reasoning.

Worse, the primary agent often runs multi-step git sequences (check status, stage files, commit, check merged status, push, create PR) where each step requires a separate tool call and round-trip. Each round-trip costs planning tokens on top of the output tokens.

The existing `delegate-git-operations` proposal addresses giving delegates git capabilities for parallel branch work. This proposal is complementary: it focuses on removing git from the primary agent's context entirely by providing high-level MCP tools that handle the full lifecycle internally and return only compact results.

## Goals

- Primary agent never runs raw `git` commands via Bash
- All git operations available as MCP tools that return compact, structured results
- Multi-step workflows (stage + commit + push) collapsed into single tool calls
- Token savings of 70-90% on git-related context vs. current approach
- Pre-hook intercepts and blocks raw git bash commands, guiding the agent to use MCP tools

## Approach

### New MCP Tools

Add seven git tools to `aimee-mcp`. Each tool runs git internally and returns a compact summary. All tools operate in the caller's working directory (respecting worktree isolation).

#### 1. `git_status`

Returns a structured summary of the working tree.

**Parameters:** none

**Returns:**
```
branch: main (ahead 2, behind 0)
staged: 3 files (src/foo.c, src/bar.c, Makefile)
modified: 1 file (src/baz.c)
untracked: 2 files (test_new.c, TODO)
```

Internal: runs `git status --porcelain=v2 --branch`, parses, compresses.

#### 2. `git_commit`

Stages specified files (or all modified), commits with the given message, returns the commit hash.

**Parameters:**
- `message` (string, required): commit message
- `files` (array of strings, optional): files to stage. If omitted, stages all modified/deleted tracked files (not untracked).

**Returns:**
```
committed: a1b2c3d "Fix memory leak in parser"
3 files changed, 47 insertions(+), 12 deletions(-)
```

Internal: runs `git add <files>`, `git commit -m <message>`, `git diff --stat HEAD~1`. Never adds untracked files unless explicitly listed. Never adds .env or credential files.

#### 3. `git_push`

Pushes the current branch to its upstream (or sets upstream on first push).

**Parameters:**
- `force` (boolean, optional, default false): use `--force-with-lease` (never `--force`)

**Returns:**
```
pushed: main -> origin/main (a1b2c3d)
```

Internal: checks if branch has upstream, runs `git push -u origin <branch>` or `git push`. On failure, returns the error.

#### 4. `git_branch`

Creates, switches, or lists branches.

**Parameters:**
- `action` (string, required): one of "create", "switch", "list", "delete"
- `name` (string, optional): branch name (required for create/switch/delete)
- `base` (string, optional): base ref for create (default: current HEAD)

**Returns (create):**
```
created: aimee/new-feature from main (a1b2c3d)
switched to aimee/new-feature
```

**Returns (list):**
```
* main (a1b2c3d)
  aimee/feature-1 (b2c3d4e)
  aimee/feature-2 (c3d4e5f)
```

#### 5. `git_log`

Returns a compact commit log.

**Parameters:**
- `count` (integer, optional, default 10): number of commits
- `ref` (string, optional): ref or range (e.g. "main..HEAD")
- `diff_stat` (boolean, optional, default false): include diffstat per commit

**Returns:**
```
a1b2c3d 2h ago  Fix memory leak in parser
b2c3d4e 5h ago  Add retry logic to delegate
c3d4e5f 1d ago  Initial MCP server implementation
```

Internal: runs `git log --oneline --format=...` with relative dates. Caps output at `count` entries.

#### 6. `git_diff_summary`

Returns a compact summary of changes without the full diff.

**Parameters:**
- `ref` (string, optional): compare against this ref (default: HEAD for unstaged, or staged changes)
- `stat_only` (boolean, optional, default true): only show file-level stats
- `files` (array of strings, optional): limit to these files

**Returns (stat_only=true):**
```
3 files changed, 47 insertions(+), 12 deletions(-)
  src/foo.c     | 32 +++++++++---
  src/bar.c     | 15 +++--
  Makefile       |  12 +++
```

**Returns (stat_only=false, specific file):**
```
src/foo.c (32 insertions, 5 deletions):
  +  added: error handling in parse_token()
  +  added: null check before dereference
  -  removed: unused variable 'tmp'
  ~  modified: return type of validate()
```

When `stat_only=false`, aimee internally runs `git diff` but compresses it to a change summary using pattern matching (added/removed/modified function signatures, struct fields, etc.) rather than returning raw unified diff. This is where the biggest token savings come from.

#### 7. `git_pr`

Creates or inspects a pull request via `gh`.

**Parameters:**
- `action` (string, required): "create", "view", "list", "merge_status"
- `title` (string, optional): PR title (for create)
- `body` (string, optional): PR body (for create)
- `number` (integer, optional): PR number (for view/merge_status)
- `base` (string, optional): base branch (for create, default: main)

**Returns (create):**
```
created: PR #142 "Fix memory leak in parser"
url: https://github.com/user/repo/pull/142
base: main <- aimee/fix-memleak
```

**Returns (merge_status):**
```
PR #142: merged (2024-03-15)
```

This is critical for the recurring "check if PR is merged before pushing" pattern that has caused repeated issues.

### Pre-Hook Interception

Modify `pre_tool_check()` in `guardrails.c` to block raw git commands from Bash and emit guidance:

```c
/* In pre_tool_check, after existing git_write_cmds detection */
if (is_git_command(command)) {
   snprintf(msg, msg_len,
      "Use aimee MCP git tools instead of raw git commands. "
      "Available: git_status, git_commit, git_push, git_branch, "
      "git_log, git_diff_summary, git_pr");
   return 2; /* block with guidance */
}
```

The `is_git_command()` function matches any command starting with `git ` (both read and write operations). This is stricter than the current `is_write_command()` which only catches git write commands.

Return code 2 (vs 1 for hard block) signals "blocked with suggestion" so the primary agent can retry with the correct tool.

### Changes

| File | Change |
|------|--------|
| `src/mcp_server.c` | Add 7 tool definitions in `build_tools_list()`, 7 handler functions, 7 dispatch entries in `tools/call` |
| `src/mcp_git.c` | New file: git tool implementations (status, commit, push, branch, log, diff_summary, pr) |
| `src/mcp_git.h` | New file: header for git tool handlers |
| `src/guardrails.c` | Add `is_git_command()`, modify `pre_tool_check()` to block raw git and emit MCP tool suggestion |
| `src/Makefile` | Add `mcp_git.o` to aimee-mcp build |

### Implementation Notes

**mcp_git.c** is a separate file to keep mcp_server.c from growing further. Each handler follows the same pattern:

```c
cJSON *handle_git_status(cJSON *args) {
    char output[4096];
    // Run: git status --porcelain=v2 --branch
    // Parse porcelain output into compact summary
    // Return mcp_text_content(summary)
}
```

All handlers use `popen()` like existing MCP tool handlers. Git commands run in the process's cwd (which is the session worktree when worktree isolation is active).

**Diff compression** (git_diff_summary with stat_only=false): rather than LLM-based summarization (which would add latency and cost), use heuristic pattern matching:
- Extract function/struct signatures from added/removed lines
- Group consecutive changes into "modified block" descriptions
- Cap per-file summary at 10 lines
- Total output capped at 2KB

This gives the primary agent enough to understand *what* changed without seeing every line.

## Acceptance Criteria

- [ ] `mcp__aimee__git_status` returns branch, staged, modified, untracked counts and file lists
- [ ] `mcp__aimee__git_commit` stages files, commits, returns hash and diffstat in under 200 tokens
- [ ] `mcp__aimee__git_push` pushes and returns one-line confirmation
- [ ] `mcp__aimee__git_branch` can create, switch, list, delete branches
- [ ] `mcp__aimee__git_log` returns compact log with configurable count
- [ ] `mcp__aimee__git_diff_summary` returns file-level stats or compressed change descriptions
- [ ] `mcp__aimee__git_pr` can create PRs and check merge status
- [ ] Pre-hook blocks `git *` bash commands and suggests MCP tool alternatives
- [ ] Pre-hook block message is clear enough that primary agents self-correct on first retry
- [ ] Token measurement: a commit+push workflow uses <500 tokens of git context (vs. ~3000+ today)

## Owner and Effort

- **Owner:** aimee core
- **Effort:** M (mcp_git.c handlers: S, guardrail interception: XS, diff compression: S)
- **Dependencies:** None. Complementary to `delegate-git-operations` proposal (that enables delegates; this serves primary agents).
- **Priority:** P1

## Rollout and Rollback

- **Rollout:** Two phases. Phase 1: add MCP tools (additive, no breakage). Phase 2: enable pre-hook blocking of raw git (can be gated on a config flag `guardrails.block_raw_git`). This lets agents adopt MCP tools organically before enforcement.
- **Rollback:** Remove the pre-hook git blocking (one line in guardrails.c). MCP tools are additive and can be left in place.
- **Blast radius:** Phase 1: none (additive). Phase 2: all primary agents in sessions with aimee hooks. If a tool is buggy, the agent gets blocked on git and has no fallback. Mitigated by the config flag.

## Test Plan

- [ ] Unit test: each MCP git handler returns expected format for clean repo, dirty repo, conflicts
- [ ] Unit test: `git_commit` refuses to stage .env files even when explicitly listed
- [ ] Unit test: `git_push` never uses `--force` (only `--force-with-lease` when force=true)
- [ ] Unit test: `git_diff_summary` output stays under 2KB for a 500-line diff
- [ ] Integration test: full workflow (branch, edit, commit, push, create PR) using only MCP tools
- [ ] Integration test: pre-hook blocks `git status` bash command and returns guidance message
- [ ] Manual verification: run a Claude Code session with MCP tools and measure token usage vs. baseline

## Operational Impact

- **Disk/CPU:** Negligible; git commands are fast and output is small.
- **Token savings:** Estimated 70-90% reduction in git-related token consumption per session. For a session with 20 git operations, this is roughly 15-40k tokens saved.
- **Logging:** Each MCP git tool call logged in aimee's audit trail (already handled by MCP server framework).
- **New metrics:** Consider tracking `mcp_git_calls_total` and `mcp_git_tokens_saved_estimate` counters.

## Trade-offs

**Why not just delegate all git to a sub-agent?**
Delegation has overhead: spinning up an agent, routing, tool negotiation. For a single `git status` or `git commit`, the MCP tool is faster and cheaper. Delegation (per the other proposal) makes sense for complex multi-step git work like rebasing 20 PRs. MCP tools are for the routine single-operation case.

**Why block raw git instead of just offering MCP tools?**
Without blocking, primary agents will continue using raw git because it's what they're trained on. The block with guidance creates a feedback loop that teaches the agent to use MCP tools. Phase 2 gating via config flag lets us enable this gradually.

**Why heuristic diff compression instead of LLM summarization?**
Latency and cost. An LLM call to summarize a diff would take 1-3 seconds and cost tokens itself. Heuristic extraction (function signatures, struct changes) is instant and free. The primary agent can always request the full diff of a specific file via `stat_only=false` if the summary is insufficient.
