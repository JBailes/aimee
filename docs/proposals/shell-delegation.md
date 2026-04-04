# Proposal: Shell Delegation for System Logic
## Problem
Custom C logic duplicates system tools (git/find).
## Goals
- Use popen() for system utilities.
## Approach
Replace manual walking/parsing with git/find/grep calls.
