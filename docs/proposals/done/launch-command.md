# Proposal: `aimee` as workspace manager and session launcher

## Problem

Today, aimee is a tool you invoke within a session that's already running. It has no concept of:

1. **Launching a session.** The user must know which CLI to run (`claude`, `gemini`, `codex`) and run it manually.
2. **Workspace provisioning.** Setting up a multi-repo development environment (cloning repos, installing dependencies, running initial builds/tests) is handled by an external `setup.sh` specific to each project.
3. **Project context for AI tools.** Descriptions of what each sub-project does and how they relate are manually maintained in CLAUDE.md or similar files, separate from aimee.
4. **Credentials management.** Shared secrets (database passwords, API keys) needed by sub-projects are managed outside of aimee, typically via gitignored files.

These are all solved by a separate meta-repository (aicli) with a bespoke setup script. The pattern is general: any team with multiple repos, multiple languages, and AI coding tools faces the same problems. Aimee should own this.

## Goals

- `aimee` (no subcommand) launches a provider CLI session (the daily driver).
- `aimee quickstart` is the one-time entry point: provisions the workspace from scratch (install deps, clone repos, configure hooks).
- Workspace definition is declarative (a YAML file), not a bespoke shell script.
- Project descriptions are part of the workspace definition, automatically injected into AI context.
- Credentials are tracked (paths, descriptions) but never committed to git.
- Adding a new project to the workspace is a single command.
- Aimee manages its own build dependencies (make, gcc, etc.) based on the languages in the workspace.

## Approach

### 1. Bootstrapping: `setup.sh` builds aimee, then hands off to `aimee setup`

Aimee ships with a `setup.sh` that handles the chicken-and-egg problem: you need aimee built before aimee can manage anything. The script does only what aimee cannot do for itself:

1. Installs aimee's own build dependencies (gcc, make, libsqlite3-dev, libcurl4-openssl-dev).
2. Builds aimee from source.
3. Links the binary onto PATH.
4. Runs `aimee setup`.

After this one-time bootstrap, `aimee` handles everything. The script is short (~15 lines) and generic.

```bash
#!/bin/bash
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Install aimee build deps and build
apt-get update -qq
apt-get install -y -qq gcc make libsqlite3-dev libcurl4-openssl-dev git
cd "$SCRIPT_DIR/aimee/src" && make
ln -sf "$SCRIPT_DIR/aimee/aimee" /usr/local/bin/aimee

# Hand off to aimee
aimee setup
```

### 2. Workspace manifest: `aimee.workspace.yaml`

A single YAML file in the workspace root defines everything:

```yaml
name: my-workspace
provider: claude

projects:
  - name: wol
    repo: git@github.com:JBailes/wol.git
    description: "Stateless connection interface (C#/.NET). Handles telnet, TLS, WS, WSS on port 6969."
    build: dotnet build
    test: dotnet test
    language: csharp

  - name: acktng
    repo: git@github.com:ackmudhistoricalarchive/acktng.git
    description: "Legacy MUD game server (C). Being replaced by WOL."
    build: cd src && make
    test: cd src && make unit-tests
    lint: cd src && make lint
    language: c

  - name: web-personal
    repo: git@github.com:JBailes/web-personal.git
    description: "Personal website (React + Vite SPA)."
    build: npm install && npm run build
    test: npm test
    language: javascript

dependencies:
  apt:
    - postgresql
    - postgresql-client
  custom:
    - name: ".NET 9 SDK"
      check: "dotnet --list-sdks | grep -q '^9\\.'"
      install: "curl -fsSL https://dot.net/v1/dotnet-install.sh | bash -s -- --channel 9.0"

secrets:
  - name: db.conf
    path: credentials/db.conf
    description: "PostgreSQL connection string for local dev"
  - name: api-key
    path: credentials/api-key.txt
    description: "API key for the NPC intelligence service"
```

### 3. Language-based dependency resolution

Aimee auto-detects languages by scanning file extensions in each project directory (`.c`/`.h` = C, `.py` = Python, `.js`/`.ts` = JavaScript, `.go` = Go, `.lua` = Lua, `.cs` = C#, `.rs` = Rust, `.dart` = Dart). The scan is shallow (max 2 directory levels) to stay fast, and skips `node_modules`, `.git`, `vendor`, and `__pycache__`.

For each detected language, aimee installs the required toolchain automatically:

| Language | Packages installed |
|----------|-------------------|
| `c` | `build-essential` |
| `python` | `python3`, `python3-pip`, `python3-venv` |
| `javascript` | `nodejs`, `npm` |
| `go` | `golang` |
| `lua` | `liblua5.4-dev` |
| `csharp` | .NET SDK (via `dependencies.custom` entry) |
| `rust` | rustup (via `dependencies.custom` entry) |
| `dart` | dart SDK (via `dependencies.custom` entry) |

The `language` field in the manifest is optional. If set, it acts as a hint (used before the repo is cloned, or as an override). Once the directory exists, auto-detection takes over. The `dependencies.apt` section only needs non-language dependencies (databases, system services).

A project can use multiple languages (e.g., C + Lua). Auto-detection handles this naturally since it scans for all file extensions.

### 4. `aimee workspace add`: adding projects

A new command to add a project to the workspace:

```bash
aimee workspace add --repo git@github.com:JBailes/wol-realm.git \
  --description "Game engine (C#/.NET). Runs the MUD world simulation."
```

This:
1. Appends the project to `aimee.workspace.yaml`.
2. Clones the repo into the workspace root.
3. Installs any new language dependencies (if this is the first csharp project, install .NET).
4. Registers the project path in aimee's config.json workspaces[].
5. Runs the project's build/test if defined.

The `--repo` flag is required. `--description` and `--language` are optional (can be filled in later). The project name is derived from the repo URL (last path component minus `.git`).

For removing: `aimee workspace remove wol-realm` removes the entry from the manifest. It does NOT delete the cloned directory (that's destructive, let the user do it).

### 5. `aimee` (no subcommand): launch a session

The daily driver. When invoked with no subcommand:

1. Load config (and workspace manifest if present).
2. Determine the provider (manifest > config > default "claude").
3. Generate a session ID, set it in the environment.
4. `exec` the provider CLI.

No provisioning check. If the workspace isn't set up, the user needs `aimee quickstart` first. Keeping `aimee` fast and simple (no disk scanning, no apt checks) means it launches instantly.

```c
if (cmd_start >= argc)
{
   const char *provider = "claude";
   workspace_t ws;
   if (workspace_load(&ws) == 0 && ws.provider[0])
      provider = ws.provider;
   else
   {
      config_t cfg;
      config_load(&cfg);
      if (cfg.provider[0])
         provider = cfg.provider;
   }

   setenv("CLAUDE_SESSION_ID", session_id(), 1);
   execlp(provider, provider, NULL);
   fatal("could not launch '%s'", provider);
}
```

### 6. `aimee setup` / `aimee quickstart`: idempotent workspace provisioning

These are synonyms. `quickstart` is the user-friendly name for first-time use; `setup` is the name you reach for when re-provisioning. Both run the same code.

The command handles everything from zero to working:

1. **`aimee init`** (create database and config if missing).
2. **Create `aimee.workspace.yaml`** if missing (interactive: ask for workspace name and provider).
3. **Resolve language dependencies.** Scan all projects' `language` fields, map to system packages, install missing ones.
4. **Install explicit dependencies** (`dependencies.apt`, `dependencies.custom`).
5. **Clone repos.** For each project with a `repo` field, clone if not present.
6. **Register workspaces.** Update aimee's config.json `workspaces[]`.
7. **Install aimee hooks** in the provider's settings (claude, gemini, codex).
8. **Check for missing secrets.** Print warnings, do not fail.
9. **Run build/test** for each project (optional, with `--build` flag).

Idempotent: safe to re-run at any time. Skips already-installed packages and already-cloned repos. This is what `setup.sh` calls after building aimee, and what a user runs manually after cloning a repo that already has an `aimee.workspace.yaml`.

### 7. Project context injection

During `session-start`, `build_session_context()` reads the workspace manifest and injects project descriptions:

```
# Workspace: my-workspace
Projects:
- wol: Stateless connection interface (C#/.NET). Handles telnet, TLS, WS, WSS on port 6969.
- acktng: Legacy MUD game server (C). Being replaced by WOL.
- web-personal: Personal website (React + Vite SPA).

Credentials (gitignored):
- db.conf (credentials/db.conf): PostgreSQL connection string for local dev
- api-key (credentials/api-key.txt): API key for the NPC intelligence service
```

### 8. Credentials tracking

The `secrets[]` section tracks what credentials exist, where they live, and what they're for. Aimee:
- Warns on `aimee setup` if any are missing.
- Never reads contents, commits them, or transmits them.
- Includes descriptions in session context so the AI knows they exist and where to find them.
- The paths are relative to the workspace root and expected to be gitignored.

### 9. Relationship to existing config

| Existing | New | Relationship |
|----------|-----|-------------|
| `config.json` workspaces[] | `aimee.workspace.yaml` projects[] | Workspace manifest is the source of truth. `aimee setup` syncs config.json from it. |
| `config.json` provider | `aimee.workspace.yaml` provider | Manifest takes precedence. Config.json is the fallback for workspaces without a manifest. |
| `.aimee/project.yaml` | `aimee.workspace.yaml` projects[].build/test | Per-project yaml overrides workspace-level defaults. |
| `aimee setup` (current) | `aimee setup` (new) | Current `aimee setup` only saves config. New version does full provisioning. |

## Affected Files

| File | Change |
|------|--------|
| `main.c` | No-subcommand path: load workspace, provision if needed, exec provider CLI. |
| `workspace.c` (new) | Manifest parsing, provisioning, language dependency resolution, context generation. |
| `headers/workspace.h` (new) | Workspace types and declarations. |
| `cmd_core.c` | Expand `cmd_setup` to full workspace provisioning (also aliased as `quickstart`). Add `cmd_workspace` for add/remove. |
| `cmd_hooks.c` | `build_session_context()` injects project descriptions and credentials info. |
| `main.c` | Register `workspace` and `quickstart` commands. No-subcommand path execs provider CLI. |
| `Makefile` | Add `workspace.c` to SRCS. |
| `setup.sh` (new/rewrite) | Generic bootstrap script (~30 lines). |

## YAML Parsing

Simple line-based parser for the restricted subset we need (string values, flat lists, one level of nesting under `projects[]`). No anchors, no flow style, no multi-line scalars. ~200 lines of custom C. This avoids adding an external dependency.

## Trade-offs

**Pro:**
- Single entry point: `aimee` provisions and launches.
- Declarative workspace replaces bespoke shell scripts.
- Language-aware dependency resolution: users declare languages, aimee handles toolchains.
- `aimee workspace add` makes growing a workspace trivial.
- Project descriptions and credentials travel with the manifest.
- Provider-agnostic.

**Con:**
- `aimee` with no args no longer prints usage (use `aimee --help`).
- Language-to-package mapping must be maintained as new languages are added.
- Workspace provisioning needs root/sudo for apt operations. Aimee detects this and either runs with sudo or warns.
- Custom YAML parser limits what the manifest can express (no complex YAML features). Acceptable for this use case.

## Command Summary

| Command | Purpose | Frequency |
|---------|---------|-----------|
| `aimee` | Launch a provider CLI session | Daily |
| `aimee setup` / `aimee quickstart` | Provision workspace from manifest (synonyms) | Once, or after manifest changes |
| `aimee workspace add` | Add a project to the manifest and clone it (creates manifest if missing) | As needed |
| `aimee workspace remove` | Remove a project from the manifest | As needed |

## Migration Path

1. Add workspace manifest parser (`workspace.c`).
2. Add language dependency mapping.
3. Implement `aimee setup`/`quickstart` for full provisioning.
4. Implement `aimee workspace add/remove`.
5. Update `main.c` for no-subcommand launch.
6. Update `build_session_context()` for project/credentials context.
7. Write generic `setup.sh` bootstrap script.
8. Write `aimee.workspace.yaml` for the aicli repo as the first consumer.
