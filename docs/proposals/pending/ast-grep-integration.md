# Proposal: AST-Grep Integration for Code Search

## Problem

Aimee's current code search is text-based (grep/regex). This works for simple queries but fails for structural patterns: "find all functions that take a `const char *` and return `int`", "find all `if` blocks that check `err != NULL`". Regex can approximate these but is fragile, language-unaware, and produces false positives.

Evidence: oh-my-openagent integrates ast-grep (`src/tools/ast-grep/`) as an MCP tool. It provides AST-aware pattern matching across 25+ languages using meta-variables (`$VAR` for single nodes, `$$$` for multiple). This enables precise structural queries impossible with regex.

## Goals

- Provide AST-aware code search as an MCP tool
- Support at least C, Python, JavaScript, TypeScript, Go
- Meta-variable syntax for structural patterns
- Fallback to text grep when ast-grep binary is unavailable

## Approach

Bundle the ast-grep binary and expose it as an MCP tool. The tool takes a pattern string, target language, and optional path restrictions. It runs `sg --pattern <pat> --lang <lang>` and formats the results.

### Example queries

| Pattern | Language | Matches |
|---------|----------|---------|
| `if ($COND) { return NULL; }` | C | All early-return-null guards |
| `def $FUNC($$$):` | Python | All function definitions |
| `console.log($MSG)` | JavaScript | All console.log calls |

### Changes

| File | Change |
|------|--------|
| `src/mcp_tools.c` | Add `ast_grep_search` tool definition and handler |
| `src/index.c` | Optional: use ast-grep as secondary index backend |
| `install.sh` | Bundle ast-grep binary for linux-x64 |

## Acceptance Criteria

- [ ] `ast_grep_search` tool is available via MCP
- [ ] Pattern matching works for C and Python at minimum
- [ ] Meta-variables (`$VAR`, `$$$`) work as documented
- [ ] Missing ast-grep binary returns a clear error, not a crash
- [ ] Results include file path, line number, and matched code

## Owner and Effort

- **Owner:** aimee
- **Effort:** M–L (3–5 days)
- **Dependencies:** ast-grep binary must be available for linux-x64

## Rollout and Rollback

- **Rollout:** Binary bundled with install; tool appears in MCP tool list
- **Rollback:** Remove binary and tool definition; grep remains the only search tool
- **Blast radius:** Additive — new tool only, no changes to existing tools

## Test Plan

- [ ] Unit test: C function pattern matches correctly
- [ ] Unit test: Python class pattern matches correctly
- [ ] Unit test: invalid pattern returns helpful error
- [ ] Unit test: missing binary returns clean error
- [ ] Integration test: agent uses ast-grep to find code patterns

## Operational Impact

- **Metrics:** ast-grep query count, results per query
- **Logging:** Log queries at debug level
- **Disk/CPU/Memory:** ast-grep binary ~10MB; query CPU proportional to codebase size

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| AST-Grep Integration | P3 | M–L | Medium — enables structural code queries |

## Trade-offs

Alternative: build AST parsing into Aimee's index natively. Much higher effort and maintenance burden for marginal benefit over the ast-grep binary, which is well-maintained and supports 25+ languages out of the box.

Alternative: tree-sitter queries instead of ast-grep. Tree-sitter is more flexible but requires writing queries in S-expression syntax, which is harder for agents to generate. ast-grep's pattern syntax is closer to actual code.

Inspiration: oh-my-openagent `src/tools/ast-grep/`
