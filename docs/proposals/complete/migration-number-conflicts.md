# Proposal: Detect and Normalize Migration ID Conflicts

## Problem

`src/db.c` still uses sequential migration numbers. Changing the numbering
scheme now would create churn across the migration table without fixing the real
workflow problem: the user and their autonomous agents still need a safe way to add
migrations on parallel local branches and catch duplicates before they are merged
into main.

The value-add is not a brand new numbering system. It is an explicit workflow
that detects duplicate IDs early and provides a guided renumber path.

## Goals

- Duplicate migration IDs are detected before local branches are merged
- Renumbering a conflicting migration is easy and deliberate
- Existing sequential numbering stays intact

## Approach

### 1. Add duplicate-ID validation

Add a repo-owned check that scans the migration table and fails if IDs are not:

- unique
- strictly increasing

This can live in unit tests and in a small helper command.

### 2. Add a helper for the next migration number

Add a lightweight helper such as `aimee db next-migration` that prints the next
available integer from `src/db.c`. This reduces accidental reuse when creating a
new migration.

### 3. Document the merge-time renumber workflow

When a conflict still occurs, the user (or orchestrating agent) updates only the
incoming branch's migration ID and reruns the validation. This keeps the readable
integer scheme without adopting timestamps or category ranges.

## Changes

| File | Change |
|------|--------|
| `src/tests/test_db.c` | Add migration ordering and uniqueness checks |
| `src/cmd_core.c` or new db helper | Add `next-migration` helper |
| `docs/COMMANDS.md` | Document the migration authoring workflow |

## Acceptance Criteria

- [ ] Duplicate migration IDs fail a repo-owned validation step
- [ ] `aimee db next-migration` reports the next available ID
- [ ] Existing sequential migration numbering remains unchanged

## Owner and Effort

- **Owner:** aimee core
- **Effort:** S
- **Priority:** P1

