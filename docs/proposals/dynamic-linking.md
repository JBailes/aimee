# Proposal: Dynamic Linking Standard
## Problem
Static linking of curl/ssl/sqlite inflates binary.
## Goals
- Enforce dynamic linking policy.
## Approach
Update build config to prefer dynamic system libs.
