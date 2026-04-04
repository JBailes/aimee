# Proposal: Memory Module Consolidation

## Problem
The memory subsystem is fragmented across six source files: `memory.c`, `memory_promote.c`, `memory_context.c`, `memory_scan.c`, `memory_graph.c`, and `memory_advanced.c`. This fragmentation leads to redundant includes, circular dependency risks, and difficulty in tracing the 4-tier memory lifecycle (L0-L3).

## Goals
- Consolidate the memory subsystem into three cohesive modules.
- Reduce subsystem line count by ~15% through boilerplate elimination.

## Approach
Merge the existing six files into three logical units:
1. **`memory_core.c`**: Core CRUD operations, SQLite schema management, and FTS5 search implementations.
2. **`memory_logic.c`**: The promotion/demotion pipeline, context assembly for agents, and conversation scanning.
3. **`memory_advanced.c`**: Complex features including the entity-relationship graph, anti-pattern detection, and contradiction analysis.

## Acceptance Criteria
- [ ] Memory subsystem reduced from 6 files to 3.
- [ ] All 45+ memory-related unit tests pass.
- [ ] FTS5 search remains performant (p99 < 20ms).
