# Proposal: Unified String & JSON Fluent API

## Problem
Aimee's current handling is fragmented across dstr.c, text.c, and cJSON.c, each requiring its own boilerplate. This leads to redundant code when mapping database results to JSON.

## Goals
- Create a unified Fluent API for string and JSON operations.
- Standardize on a high-performance dynamic string implementation.

## Approach
Develop a cohesive helper library that provides a concise, chainable API. Includes helpers like json_from_db_row() and str_append_fmt(). This eliminates hundreds of lines of fragmented conversion code.

## Acceptance Criteria
- [ ] Fluent API adopted in at least 3 major modules.
- [ ] Reduced line count in render.c, db.c, and server_state.c.
- [ ] Memory leaks strictly prevented.
