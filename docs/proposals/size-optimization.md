# Proposal: Aggressive Binary Size Optimization

## Problem
Current build flags are not optimized for size.

## Goals
- Reduce binary size by 40-50%.
- Use size-focused compiler optimizations.

## Approach
Switch production to -Os and enable -flto. Use linker flags like -Wl,--gc-sections to strip unreachable code and symbols.

## Acceptance Criteria
- [ ] aimee-server binary size < 1.4MB.
- [ ] All tests pass under -Os and -flto.
