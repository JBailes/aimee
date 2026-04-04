# Proposal: Config and Schema Validation with Strict Mode

## Problem

Configuration loading (`config.c:80-179`) performs basic `cJSON_IsString` checks
but has no formal schema validation. Issues:

1. **Typos are silent.** A misspelled key (e.g., `"provder"` instead of
   `"provider"`) is silently ignored — the default is used with no warning.
2. **Type errors are silent.** Setting `"use_builtin_cli": "yes"` instead of
   `"use_builtin_cli": 1` falls through to the default without error.
3. **No workspace manifest validation.** Workspace definitions, agent configs,
   and policy files (`aimee-policy.json`) are loaded with ad-hoc checks spread
   across multiple files.
4. **No fail-fast mode.** In CI/automation, misconfigured aimee silently uses
   defaults, causing subtle behavior changes that are hard to diagnose.

## Goals

- Known config keys are validated for type and value constraints.
- Unknown keys produce a warning (or error in strict mode).
- Agent configs and workspace manifests are validated at load time.
- Strict mode (`--strict` or `AIMEE_STRICT=1`) fails fast on any validation error.

## Approach

### 1. Config schema definition

Define a compile-time schema for `config.json`:

```c
typedef struct {
    const char *key;
    enum { SCHEMA_STRING, SCHEMA_INT, SCHEMA_BOOL, SCHEMA_OBJECT } type;
    int required;
    const char *default_value;
    const char *description;
} config_schema_entry_t;

static const config_schema_entry_t config_schema[] = {
    {"provider",        SCHEMA_STRING, 0, "claude",    "AI provider"},
    {"db_path",         SCHEMA_STRING, 0, NULL,        "Database path"},
    {"guardrail_mode",  SCHEMA_STRING, 0, "approve",   "Guardrail mode"},
    {"use_builtin_cli", SCHEMA_INT,    0, "0",         "Use builtin CLI"},
    {"openai_endpoint", SCHEMA_STRING, 0, NULL,        "OpenAI endpoint"},
    {"openai_model",    SCHEMA_STRING, 0, "gpt-4o",    "OpenAI model"},
    {"openai_key_cmd",  SCHEMA_STRING, 0, NULL,        "API key command"},
    /* ... */
    {NULL, 0, 0, NULL, NULL}
};
```

### 2. Validation pass

After `cJSON_Parse`, iterate config keys and:
1. Check each key exists in schema (unknown keys → warning or error).
2. Check type matches (string vs int vs bool vs object).
3. Check value constraints where applicable (e.g., `guardrail_mode` must be one
   of `approve`/`prompt`/`deny`).

### 3. Strict mode

When `--strict` flag or `AIMEE_STRICT=1` is set:
- Unknown keys are errors (not warnings).
- Type mismatches are errors.
- Missing required keys are errors.
- Exit with non-zero code and clear diagnostics.

Normal mode: same checks, but warnings to stderr instead of errors.

### 4. Validation for other config files

Apply same pattern to:
- Agent configs (validated in `agent_config.c`)
- Policy files (`agent_policy.c`)
- Workspace manifests (`workspace.c`)

### Changes

| File | Change |
|------|--------|
| `src/config.c` | Add schema table, validation pass in `config_load()`, strict mode |
| `src/config.h` | Export schema for test access, add strict mode flag |
| `src/agent_config.c` | Add schema validation for agent definitions |
| `src/agent_policy.c` | Add schema validation for policy files |
| `src/main.c` | Parse `--strict` flag and `AIMEE_STRICT` env |

## Acceptance Criteria

- [ ] Typos in config keys produce warnings (or errors in strict mode)
- [ ] Type mismatches are detected and reported with expected vs actual type
- [ ] `--strict` / `AIMEE_STRICT=1` exits non-zero on any validation error
- [ ] Agent configs and policy files are validated at load time
- [ ] Normal mode loads successfully with warnings (backward compatible)

## Owner and Effort

- **Owner:** TBD
- **Effort:** M
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Ships with next binary. Default mode is permissive (warnings only).
- **Rollback:** Revert commit. Validation removed; silent defaults restored.
- **Blast radius:** Normal mode: no behavior change except new stderr warnings. Strict mode: existing configs with typos will fail.

## Test Plan

- [ ] Unit test: valid config passes validation
- [ ] Unit test: unknown key produces warning
- [ ] Unit test: type mismatch produces warning with expected/actual
- [ ] Unit test: strict mode rejects unknown keys and type mismatches
- [ ] Integration test: `aimee --strict` with invalid config exits non-zero

## Operational Impact

- **Metrics:** None.
- **Logging:** Validation warnings/errors to stderr.
- **Alerts:** None.
- **Disk/CPU/Memory:** Negligible — one-time validation at startup.

## Priority

P1 — fail-fast on bad config prevents subtle runtime behavior changes.

## Trade-offs

**Why compile-time schema instead of external JSON schema?** The config is simple
enough that a C struct table is clearer and has zero runtime parsing cost. An
external JSON schema file adds a dependency and a file to keep in sync.

**Why warnings by default instead of errors?** Backward compatibility. Existing
users may have experimental or deprecated keys. Warnings notify without breaking.
