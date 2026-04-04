# Proposal: Shared Logic Library Transition

## Problem
Static binaries duplicate common code layers, leading to unnecessary disk usage.

## Goals
- Transition to libaimee.so for common logic.
- Reduce disk footprint by > 3MB.

## Approach
Refactor the Makefile for libaimee.so. Both client and server will link dynamically, ensuring common logic is stored once. Update install for shared library paths.

## Acceptance Criteria
- [ ] libaimee.so used by both binaries.
- [ ] Disk footprint reduced by > 3MB.
- [ ] ldd check verifies dynamic linkage.
