# Proposal: Fix Shell Injection and Command Construction in MCP Handlers

## Problem

The MCP server has two shell injection vulnerabilities and one command construction bug:

1. **CRITICAL — `mcp_server.c:761` `handle_delegate_reply_mcp`:** Neither `delegation_id` nor `content` is shell-escaped before being interpolated into a `popen()` command:
   ```c
   snprintf(cmd, sizeof(cmd), "aimee --json delegate reply --id '%s' --content '%s' 2>&1",
            jid->valuestring, jcontent->valuestring);
   ```
   A malicious MCP client can inject arbitrary shell commands via `content` containing single quotes (e.g., `'; rm -rf / #`).

2. **HIGH — `mcp_server.c:811` `handle_delegate`:** The `role` parameter (`jr->valuestring`) is not shell-escaped:
   ```c
   snprintf(cmd, sizeof(cmd), "aimee --json delegate %s --tools '%s' 2>&1",
            jr->valuestring, escaped);
   ```
   The prompt IS escaped, but the role is passed bare — it's also injectable. Additionally, the prompt is passed as `--tools` flag value, but `--tools` is declared as a **boolean flag** in `cli_client.c:847`. The prompt only works by accident: the bool-flag parser consumes `--tools` as `"1"`, and the prompt text falls through as `positional[1]`. This means `--tools` is silently set to `"1"` on every MCP delegation as an unintended side effect.

3. **MEDIUM — `mcp_git.c`:** 25+ shell commands are constructed via `snprintf` + `popen`. While `mcp_git.c` uses `shell_escape()` consistently for user inputs, `popen()` invokes a shell, making it inherently riskier than the `safe_exec_capture()` function already declared in `util.h:85` which uses `fork/exec` without a shell. The comment at `webchat.c:586` explicitly notes "Use safe argv execution instead of popen() to prevent shell injection" — the project is aware but hasn't migrated `mcp_git.c`.

## Goals

- Zero shell injection vectors in MCP handlers
- MCP delegate command construction is correct (prompt as positional arg, not `--tools`)
- Path toward eliminating `popen()` for user-influenced commands

## Approach

### Phase 1: Immediate Security Fix (P0)

**Fix `handle_delegate_reply_mcp` (`mcp_server.c:761`):**
Shell-escape both `delegation_id` and `content` using `shell_escape()`:
```c
char *esc_id = shell_escape(jid->valuestring);
char *esc_content = shell_escape(jcontent->valuestring);
snprintf(cmd, sizeof(cmd), "aimee --json delegate reply --id '%s' --content '%s' 2>&1",
         esc_id, esc_content);
free(esc_id);
free(esc_content);
```

**Fix `handle_delegate` (`mcp_server.c:811`):**
Shell-escape the role parameter AND fix the command to pass prompt as positional arg:
```c
// Before (broken):
snprintf(cmd, sizeof(cmd), "aimee --json delegate %s --tools '%s' 2>&1",
         jr->valuestring, escaped);

// After (correct):
char *esc_role = shell_escape(jr->valuestring);
snprintf(cmd, sizeof(cmd), "aimee --json delegate '%s' '%s' 2>&1",
         esc_role, escaped);
free(esc_role);
```

### Phase 2: Hardening — Migrate to `safe_exec_capture()` (P2)

Replace `popen()` with `safe_exec_capture()` for all user-influenced commands in:
- `mcp_server.c` delegate handlers
- `mcp_git.c` (all 25+ git commands)
- `git_verify.c`
- `cmd_branch.c`

This eliminates the shell entirely, making `shell_escape` unnecessary for these paths. Each `popen` call becomes an `argv[]` array passed to `safe_exec_capture()`:
```c
// Before:
snprintf(cmd, sizeof(cmd), "git checkout '%s' 2>&1", esc_name);
FILE *fp = popen(cmd, "r");

// After:
const char *argv[] = {"git", "checkout", name, NULL};
char *output = NULL;
int rc = safe_exec_capture(argv, &output, MAX_CAPTURE_SIZE);
```

### Changes

| File | Change |
|------|--------|
| `src/mcp_server.c:761` | Shell-escape `delegation_id` and `content` in `handle_delegate_reply_mcp` |
| `src/mcp_server.c:811` | Shell-escape `role`, change `--tools` to positional arg in `handle_delegate` |
| `src/tests/test_mcp_server.c` | Add injection test cases for delegate and delegate_reply |
| `src/mcp_git.c` (phase 2) | Replace `popen()` + `run_cmd()` with `safe_exec_capture()` for all git commands |
| `src/mcp_server.c` (phase 2) | Replace `popen()` with `safe_exec_capture()` in delegate handlers |
| `src/git_verify.c` (phase 2) | Replace `popen()` + `run_cmd()` with `safe_exec_capture()` |
| `src/cmd_branch.c` (phase 2) | Replace `popen()` + `run_cmd()` with `safe_exec_capture()` |

## Acceptance Criteria

- [ ] Payloads containing `'`, `;`, `|`, `` ` ``, `$()` in `delegation_id`, `content`, `role`, and `prompt` do not execute arbitrary commands
- [ ] `aimee delegate` via MCP produces identical results to CLI invocation (role and prompt arrive correctly)
- [ ] `--tools` flag is no longer accidentally set on MCP delegations
- [ ] `test_mcp_server.c` includes injection test cases with shell metacharacters
- [ ] (Phase 2) Zero `popen()` calls with user-influenced arguments in `mcp_git.c` and `mcp_server.c`

## Owner and Effort

- **Owner:** aimee core
- **Effort:** S (phase 1), L (phase 2)
- **Dependencies:** Phase 1 can use inline `shell_escape()` or depend on the DRY proposal for the shared version

## Rollout and Rollback

- **Rollout:** Phase 1 — compile and deploy immediately. Phase 2 — incremental, one file at a time.
- **Rollback:** Revert commit
- **Blast radius:** Phase 1 — MCP delegate tool calls only, low risk (escaping doesn't change happy-path behavior). Phase 2 — all MCP git operations, medium risk (requires careful testing of each git command).

## Test Plan

- [ ] Unit tests: injection payloads in `delegation_id`, `content`, `role`, `prompt` (test_mcp_server.c)
- [ ] Integration tests: MCP delegate round-trip with prompts containing special characters
- [ ] Failure injection: malformed role strings, oversized prompts
- [ ] Manual verification: `aimee delegate` via MCP with `role="review"` and complex prompts matches CLI behavior

## Operational Impact

- **Metrics:** None
- **Logging:** None
- **Alerts:** None
- **Disk/CPU/Memory:** Negligible (malloc/free for escape buffers)

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Phase 1: Fix injection in delegate handlers | P0 | S | Closes actively exploitable injection vectors |
| Phase 2: Migrate popen to safe_exec_capture | P2 | L | Defense in depth, eliminates shell from attack surface |

## Trade-offs

Phase 1 is a band-aid — shell escaping is inherently fragile and depends on correct quoting context. Phase 2 (`safe_exec_capture`) is the proper fix but requires converting ~30 `popen` call sites with careful `argv` construction. Doing both phases ensures immediate safety while working toward the architecturally correct solution.

An alternative to Phase 2 is using `posix_spawn()` instead of `safe_exec_capture()`, but `safe_exec_capture` already exists and is tested.
