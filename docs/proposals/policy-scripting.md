# Proposal: Guardrail Scripting Engine

## Problem
Hard-coded safety guardrails in C are inflexible and require re-compilation to modify. This makes it difficult to customize policies.

## Goals
- Move policy logic out of C and into readable scripts.
- Embed a tiny scripting engine (e.g., Lua) for rule interpretation.

## Approach
Introduce a lightweight scripting engine (e.g., Lua). Refactor the guardrail engine to load policies from guardrails.json or a Lua script. Evaluates ruleset at runtime, providing a clear "source of truth" for policy that is easily editable.

## Acceptance Criteria
- [ ] Guardrail rules successfully moved to external script.
- [ ] Rules engine correctly blocks sensitive files.
- [ ] Policy changes applied without recompilation.
