# Proposal: Workspace-Scoped Memory Recall at Session Start

## Problem

When a session starts, `cmd_session_start` outputs rules, condensed CLAUDE.md, top 5 L2 facts, and delegation hints. But the memory recall is not workspace-aware: it always returns the same top 5 facts regardless of which project directory the session started in. This means starting a session in `aimee/` shows the same facts as starting in `wol-realm/`, losing the opportunity to prime the context with project-relevant knowledge.

## Approach

Enhance `build_session_context()` in `cmd_hooks.c` to detect the current working directory, match it against configured workspaces, and include project-specific memories.

### Changes

**`cmd_hooks.c` (`build_session_context`)**:

1. Get `cwd` via `getcwd()`
2. Match `cwd` against `cfg.workspaces[]` to identify the active project name
3. If a project match is found, query memories that reference that project:
   - Search `memories` table for entries where `key` or `content` contains the project name
   - Query `terms` table for a quick project summary (file count, top exported symbols)
4. Append a `# Project Context` section to the session output with:
   - Project name and root path
   - Top 3 project-relevant memories (facts/decisions with the project name in key or content)
   - Count of indexed files and symbols for that project

### Example output

```
# Project Context (aimee)
- aimee is a C tool for AI coding assistant memory, indexing, and guardrails
- aimee uses SQLite with 23 migrations, FNV-1a keyed statement cache
- 32 source files indexed, 247 definitions
```

### Workspace matching logic

```c
/* Match cwd against workspaces */
const char *project_name = NULL;
for (int i = 0; i < cfg.workspace_count; i++) {
    if (strncmp(cwd, cfg.workspaces[i], strlen(cfg.workspaces[i])) == 0) {
        /* Extract project name from last path component */
        const char *slash = strrchr(cfg.workspaces[i], '/');
        project_name = slash ? slash + 1 : cfg.workspaces[i];
        break;
    }
}
```

## Trade-offs

- Adds ~5 database queries to session start. These are lightweight SELECT queries against indexed data, so the impact should be negligible (under 10ms on SQLite).
- Project matching is path-prefix based, which won't work if the user starts a session in a subdirectory not listed in workspaces. This is acceptable since workspaces are explicitly configured.
- The project context section adds to the session start output, which is already bounded by MAX_CONTEXT_TOTAL. May need to adjust the budget to accommodate the new section without crowding out rules.

## Testing

- Verify that starting in a workspace directory includes project-specific memories
- Verify that starting outside any workspace omits the project context section
- Verify the output stays within MAX_CONTEXT_TOTAL
