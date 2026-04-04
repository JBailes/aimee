# Proposal: Lean Build Profile

## Problem
No easy way to build a minimal core-only version.

## Goals
- Create a 'make lean' build target.
- Minimal binary under 1.5MB.

## Approach
Implement a 'make lean' target that automatically omits UI and non-essential tools. Provides a clear path for minimal deployments.

## Acceptance Criteria
- [ ] 'make lean' produces functional binary under 1.5MB.
- [ ] Core hook and memory functionality intact.
