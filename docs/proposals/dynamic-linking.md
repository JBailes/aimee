# Proposal: Dynamic Linking Standard

## Problem
Static linking of curl/ssl inflates binary size.

## Goals
- Enforce dynamic linking policy.
- Keep aimee binary lean.

## Approach
Update build to prefer dynamic linking for system-provided libs on Linux and macOS. Provide static fallback where required.

## Acceptance Criteria
- [ ] Build defaults to dynamic linkage for system libs.
- [ ] Reduced binary size for aimee client.
