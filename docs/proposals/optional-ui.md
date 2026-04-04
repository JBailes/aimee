# Proposal: Optional UI Components
## Problem
Web UI bloats the server binary.
## Goals
- Move UI behind WITH_UI flag.
## Approach
Wrap dashboard.c and webchat.c in #ifdef.
