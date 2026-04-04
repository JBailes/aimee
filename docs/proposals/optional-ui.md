# Proposal: Optional UI Components

## Problem
The embedded web dashboard (`dashboard.c`) and webchat interface (`webchat.c`) contribute significantly to the `aimee-server` binary size (~110KB). Many users only utilize the CLI and hooks, making these components unnecessary bloat for the default distribution.

## Goals
- Move UI components behind a `WITH_UI` build flag.
- Reduce default server binary size by > 100KB.

## Approach
Wrap all UI-related source code and headers in `#ifdef WITH_UI`. Update the `Makefile` to detect the flag and conditionally compile these modules. The server will return a "UI not available" error if a client attempts to connect to the dashboard port in a non-UI build.

## Acceptance Criteria
- [ ] Default `make` produces a lean `aimee-server` without UI symbols.
- [ ] `make WITH_UI=1` produces a full binary with dashboard support.
- [ ] Binary size reduction of ~110KB in the lean server.
