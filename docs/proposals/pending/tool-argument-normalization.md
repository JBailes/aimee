# Proposal: Tool Argument Normalization and Alias Resolution

## Problem

When LLMs invoke aimee's tools (via MCP, webchat, or CLI chat), they frequently provide slightly wrong parameter names or types. Common failures include:
- Using `filepath` instead of `file_path`, `dir` instead of `directory`, `path` instead of `file_path`
- Passing a string `"5"` when an integer `5` is expected
- Providing a relative path when an absolute path is needed
- Using `True` (Python-style) instead of `true` (JSON-style) for booleans

Each of these causes a tool call failure, wasting a turn and tokens on an error message + retry.

ayder-cli implements a three-stage argument normalization pipeline (`tools/normalization.py`):
1. **Alias resolution**: Maps common parameter name variants to canonical names (e.g., `dir` → `path`, `filepath` → `file_path`)
2. **Path resolution**: Resolves relative paths to absolute paths within the project sandbox via `ProjectContext.validate_path()`
3. **Type coercion**: Converts string representations to expected types (e.g., `"5"` → `5` for integer parameters)

Tool definitions declare their aliases and path parameters:
```python
parameter_aliases: Tuple[Tuple[str, str], ...] = (("dir", "path"), ("filepath", "file_path"))
path_parameters: Tuple[str, ...] = ("file_path",)
```

This is directly applicable to aimee because:
- MCP tool calls come from Claude Code, which may use variant parameter names
- Webchat and CLI chat tool calls come from various LLM providers with different naming conventions
- Delegate agents using different models (ollama, codex) have different tool-calling accuracy

Evidence:
- `mcp_tools.c` builds tool schemas with fixed parameter names and no aliases
- `agent_tools.c` parses tool arguments with no normalization layer
- No type coercion exists — string-typed arguments are passed as-is
- Delegation attempt logs show tool call failures from parameter mismatches

## Goals

- Tool arguments are normalized before execution: aliases resolved, paths made absolute, types coerced.
- Each tool declares its parameter aliases and path parameters alongside its schema.
- Normalization happens in a single layer shared by MCP, webchat, CLI chat, and delegate tool execution.
- Failed normalization (e.g., path outside sandbox) produces a clear error before execution.
- Normalization is invisible to the LLM — it sees the same tool schema, but more of its calls succeed.

## Approach

### 1. Alias map per tool

Extend tool definitions in `mcp_tools.c` and `agent_tools.c` with alias metadata:

```c
typedef struct {
    const char *alias;     // variant name LLMs might use
    const char *canonical; // correct parameter name
} param_alias_t;

typedef struct {
    const char *name;
    param_alias_t aliases[8];
    int alias_count;
    const char *path_params[4]; // parameters that are file paths
    int path_param_count;
} tool_normalization_t;
```

### 2. Normalization function

```c
// Normalize tool arguments in-place.
// - Resolves aliases to canonical names
// - Resolves relative paths to absolute within project root
// - Coerces string values to expected types (int, bool)
// Returns 0 on success, -1 on error (e.g., path outside sandbox).
int tool_normalize_args(cJSON *args,
                        const tool_normalization_t *norm,
                        const char *project_root);
```

### 3. Integration points

Call `tool_normalize_args()` as the first step of tool execution in all contexts:

- **MCP server** (`mcp_server.c`): Before dispatching MCP tool calls
- **Webchat** (`webchat.c`): Before executing tool calls in `wc_execute_tool_calls()`
- **CLI chat** (`cmd_chat.c`): Before executing tool calls
- **Delegates** (`agent_tools.c`): Before executing delegate tool calls

### 4. Common aliases

Define a shared alias table for common patterns LLMs use:

| Alias | Canonical | Tools |
|-------|-----------|-------|
| `filepath`, `file`, `path` | `file_path` | read_file, write_file |
| `dir`, `directory`, `folder` | `path` | list_files |
| `cmd`, `command_string` | `command` | bash |
| `content`, `text`, `body` | `content` | write_file |
| `query`, `search_query`, `q` | `query` | search_memory |

### 5. Path resolution

For parameters declared as path parameters, resolve relative paths:

```c
// "src/main.c" → "/root/dev/aimee/src/main.c"
// "../etc/passwd" → error (outside sandbox)
```

### Changes

| File | Change |
|------|--------|
| `src/tool_normalize.c` (new) | Normalization pipeline: aliases, paths, type coercion |
| `src/headers/tool_normalize.h` (new) | Public normalization API and alias/path metadata |
| `src/mcp_tools.c` | Add alias/path metadata per tool, call normalize before dispatch |
| `src/mcp_server.c` | Call normalize before MCP tool execution |
| `src/agent_tools.c` | Call normalize before delegate tool execution |
| `src/webchat.c` | Call normalize before webchat tool execution |
| `src/cmd_chat.c` | Call normalize before CLI chat tool execution |

## Acceptance Criteria

- [ ] Alias resolution maps variant parameter names to canonical names for all tools
- [ ] Path parameters are resolved to absolute paths within the project sandbox
- [ ] Type coercion converts string integers to integers and string booleans to booleans
- [ ] Path traversal attempts (e.g., `../etc/passwd`) produce clear errors
- [ ] Normalization is applied in MCP, webchat, CLI chat, and delegate tool execution
- [ ] Tool schemas sent to LLMs remain unchanged (normalization is transparent)

## Owner and Effort

- **Owner:** delegate (code)
- **Effort:** S (1-2 days)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Normalization is always active. It only changes behavior for malformed arguments that would have failed anyway.
- **Rollback:** Remove `tool_normalize_args()` calls. Tool execution reverts to raw argument handling.
- **Blast radius:** Normalization could theoretically change a correct argument to an incorrect one (e.g., if an alias maps to the wrong canonical). Mitigation: aliases are explicit and reviewable, and normalization logs all changes at DEBUG level.

## Test Plan

- [ ] Unit tests: alias resolution (single alias, multiple aliases, no alias needed), path resolution (relative, absolute, traversal), type coercion (string→int, string→bool, no-op)
- [ ] Integration tests: MCP tool call with aliased parameter, webchat tool call with relative path
- [ ] Failure injection: path outside sandbox, invalid type coercion, unknown alias
- [ ] Manual verification: call `read_file` with `filepath` instead of `file_path`, verify it succeeds

## Operational Impact

- **Metrics:** `tool_args_normalized_total{type="alias|path|type"}`, `tool_args_normalization_errors_total`
- **Logging:** Normalization changes at DEBUG (e.g., "resolved alias 'filepath' → 'file_path'")
- **Alerts:** High normalization error rate suggests tool schemas need updating
- **Disk/CPU/Memory:** Negligible — string comparison and path resolution

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Alias resolution | P2 | S | High — directly reduces tool call failures |
| Path resolution | P2 | S | High — prevents sandbox escapes and fixes relative paths |
| Type coercion | P3 | S | Medium — less common failure mode |
| Shared integration | P2 | S | High — all execution paths benefit |

## Trade-offs

- **Why not fix the tool schemas instead?** LLMs will always produce variant parameter names regardless of schema quality. Normalization handles the long tail of LLM output variation that better schemas can't fully eliminate.
- **Why aliases instead of fuzzy matching?** Explicit aliases are predictable and auditable. Fuzzy matching could introduce subtle bugs by mapping to the wrong parameter.
- **Why resolve paths here instead of in each tool?** Centralizing path resolution ensures consistent sandbox enforcement across all tools. Individual tools don't need to re-implement path validation.

## Source Reference

Implementation reference: ayder-cli `tools/normalization.py` — `normalize_arguments()` function with alias resolution, path validation via `ProjectContext.validate_path()`, and type coercion from tool schemas.
