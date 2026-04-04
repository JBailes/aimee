# Proposal: Shell Delegation for System Logic

## Problem
Large portions of Aimee's logic replicate standard system utilities (e.g., mcp_git.c manually parsing git porcelain output, index.c manually walking directories). This C-heavy approach is complex, error-prone, and adds unnecessary lines.

## Goals
- Replace complex C implementations with popen() calls to system tools.
- Reduce code complexity and potential for bugs.

## Approach
Audit mcp_git.c, index.c, and platform_process.c. Replace custom C logic with direct calls to git status, find, and grep. This leverages optimized system binaries, significantly reducing C code. Error handling will be simplified to checking command exit status.

## Acceptance Criteria
- [ ] Line count in mcp_git.c and index.c reduced by > 40%.
- [ ] Shell-out latency remains under 5ms.
- [ ] All unit tests pass.
