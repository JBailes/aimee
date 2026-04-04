# Proposal: Local AI Delegates (CPU-bound)

## Problem

Aimee's delegate system supports local models via Ollama (e.g., `aimee agent add ollama http://localhost:11434/v1 llama3.2`), but the setup is entirely manual. Users must independently discover, install, and configure a local inference runtime before they can use it. There is no guided path from either `install.sh` or a standalone script.

This creates two problems:

1. **Friction at install time.** Users who want cheap/free delegation for routine tasks (summarization, formatting, code review, commit messages) must leave the installer, figure out Ollama or llama.cpp on their own, then manually wire it into `agents.json`. Most don't bother.

2. **No `add-local-delegate.sh` exists.** The README references local models and `aimee agent add ollama`, but there is no script that automates runtime detection, model pulling, health-checking, and agent registration. Adding a second delegate later (e.g., a different model or quantization) requires the same manual work again.

The result: local AI delegation is a documented feature that almost nobody uses because the on-ramp is missing.

## Goals

- Users can set up a working CPU-bound local delegate in under two minutes, guided by either `./install.sh` or `./add-local-delegate.sh`.
- The setup detects the available inference runtime (Ollama, llama.cpp server, or a user-supplied OpenAI-compatible endpoint) — it does not force a specific backend.
- Models are selected with sensible defaults for CPU-only hardware (small quantized models), with the option to override.
- The delegate is registered in `agents.json` with appropriate roles, cost tier, and auth settings, ready for immediate use via `aimee delegate`.
- `aimee agent test <name>` passes before the script exits.

## Approach

### 1. New script: `add-local-delegate.sh`

A standalone bash script (also callable from `install.sh`) that walks the user through setting up a local AI delegate. The flow:

```
1. Detect runtime
   ├─ Ollama installed?      → use http://localhost:11434/v1
   ├─ llama-server running?  → probe http://localhost:8080/v1
   └─ Neither?               → offer to install Ollama, or enter custom endpoint

2. Select model
   ├─ Show recommended CPU models with RAM requirements:
   │   • qwen3:1.7b       (~1.5 GB RAM, fast, good for simple tasks)
   │   • qwen3:4b         (~3.0 GB RAM, balanced)
   │   • qwen3:8b         (~5.5 GB RAM, best quality for CPU)
   │   • llama3.2:3b      (~2.5 GB RAM, good general purpose)
   │   • phi4-mini        (~2.5 GB RAM, strong reasoning)
   │   • gemma3:4b        (~3.0 GB RAM, good instruction following)
   │   • Custom (enter model name)
   └─ Default: qwen3:4b (best balance of speed/quality on CPU)

3. Pull model (Ollama only)
   ├─ ollama pull <model>
   └─ Wait for completion, show progress

4. Configure delegate
   ├─ Agent name (default: "local")
   ├─ Roles: code, review, draft, explain, refactor, summarize
   ├─ Cost tier: 0 (cheapest)
   ├─ Auth type: none
   ├─ Max tokens: 4096
   ├─ Timeout: 300000ms (5 min — CPU inference is slow)
   └─ Tools enabled: true (agentic execution)

5. Register via aimee agent add
   └─ aimee agent add local <endpoint> <model> \
        --auth-type none --roles code,review,draft,explain,refactor,summarize \
        --cost-tier 0 --tools --timeout 300000

6. Health check
   └─ aimee agent test local
```

### 2. Install.sh integration

After the existing "Choose your primary AI CLI" and "Built-in CLI configuration" sections (line ~305), add an optional step:

```bash
# --- Optional: Local AI delegate ---
echo ""
bold "Set up a local AI delegate? (CPU-only, free)"
echo "  This installs a small local model for cheap delegation tasks"
echo "  like code review, summarization, and formatting."
echo ""
read -rp "  Set up local delegate? [y/N] " local_answer
if [[ "$local_answer" =~ ^[Yy] ]]; then
    bash "$SCRIPT_DIR/add-local-delegate.sh"
fi
```

### 3. Runtime detection logic

```bash
detect_runtime() {
    # 1. Check for Ollama
    if command -v ollama &>/dev/null; then
        # Ensure Ollama is running
        if curl -sf http://localhost:11434/api/tags &>/dev/null; then
            echo "ollama"
            return 0
        else
            warn "Ollama is installed but not running. Start it with: ollama serve"
            read -rp "  Start Ollama now? [Y/n] " start_answer
            if [[ ! "$start_answer" =~ ^[Nn] ]]; then
                ollama serve &>/dev/null &
                sleep 2
                if curl -sf http://localhost:11434/api/tags &>/dev/null; then
                    echo "ollama"
                    return 0
                fi
            fi
        fi
    fi

    # 2. Check for llama-server (llama.cpp)
    if curl -sf http://localhost:8080/v1/models &>/dev/null; then
        echo "llama-cpp"
        return 0
    fi

    # 3. Nothing found
    echo "none"
    return 1
}
```

### 4. Ollama auto-install (optional)

If no runtime is detected, offer to install Ollama:

```bash
if [ "$runtime" = "none" ]; then
    echo "  No local inference runtime detected."
    echo ""
    echo "  1) Install Ollama (recommended, one command)"
    echo "  2) Enter a custom OpenAI-compatible endpoint"
    echo "  3) Skip local delegate setup"
    echo ""
    read -rp "  Select [1-3]: " runtime_choice
    case "$runtime_choice" in
        1)
            info "Installing Ollama..."
            # Download installer to temp file, verify before executing
            local installer
            installer=$(mktemp /tmp/ollama-install.XXXXXX.sh)
            if ! curl -fsSL https://ollama.com/install.sh -o "$installer"; then
                error "Failed to download Ollama installer"
                rm -f "$installer"
                exit 1
            fi
            # Verify installer is a shell script (basic sanity check)
            if ! head -1 "$installer" | grep -q '^#!/'; then
                error "Downloaded file does not appear to be a shell script"
                rm -f "$installer"
                exit 1
            fi
            chmod +x "$installer"
            bash "$installer"
            rm -f "$installer"
            ollama serve &>/dev/null &
            sleep 3
            runtime="ollama"
            ;;
        2)
            read -rp "  Endpoint URL: " custom_endpoint
            runtime="custom"
            ;;
        3) exit 0 ;;
    esac
fi
```

### 5. Model recommendation engine

The script recommends models based on available system RAM:

```bash
recommend_model() {
    local ram_gb
    ram_gb=$(awk '/MemTotal/ {printf "%.0f", $2/1024/1024}' /proc/meminfo 2>/dev/null || echo "4")

    echo ""
    bold "Select a model for CPU inference"
    echo "  System RAM: ${ram_gb} GB"
    echo ""

    if [ "$ram_gb" -ge 8 ]; then
        echo "  1) qwen3:8b       (~5.5 GB, best quality)     [recommended]"
        echo "  2) qwen3:4b       (~3.0 GB, balanced)"
        echo "  3) phi4-mini      (~2.5 GB, strong reasoning)"
        DEFAULT_MODEL_NUM=1
    elif [ "$ram_gb" -ge 4 ]; then
        echo "  1) qwen3:4b       (~3.0 GB, balanced)         [recommended]"
        echo "  2) llama3.2:3b    (~2.5 GB, general purpose)"
        echo "  3) qwen3:1.7b     (~1.5 GB, fast)"
        DEFAULT_MODEL_NUM=1
    else
        echo "  1) qwen3:1.7b     (~1.5 GB, fast)             [recommended]"
        echo "  2) gemma3:1b      (~1.0 GB, minimal)"
        DEFAULT_MODEL_NUM=1
    fi
    echo "  c) Custom (enter model name)"
}
```

### 6. Fallback chain integration

When a local delegate is added, update the fallback chain so it's tried first for cheap roles:

```json
{
  "fallback_chain": ["local", "codex", "claude", "gemini"]
}
```

This means delegation naturally routes to the free local model first. If it fails or is unavailable, it falls back to cloud agents.

### Changes

| File | Change |
|------|--------|
| `add-local-delegate.sh` (new) | Standalone script: runtime detection, model selection, pull, agent registration, health check |
| `install.sh` | Add optional local delegate step after built-in CLI config (~line 305) |
| `src/cmd_agent.c` | No changes needed — existing `agent add` and `agent test` commands handle registration |
| `src/agent_config.c` | No changes needed — existing config loading handles `auth_type: none` and local endpoints |
| `README.md` | Update "Local models" section to reference `add-local-delegate.sh` |

## Acceptance Criteria

- [ ] `./add-local-delegate.sh` on a machine with Ollama installed completes in <2 minutes and registers a working delegate
- [ ] `./add-local-delegate.sh` on a machine without Ollama offers to install it or accept a custom endpoint
- [ ] `./install.sh` offers local delegate setup as an optional step; answering N skips cleanly with no errors
- [ ] After setup, `aimee agent list` shows the local delegate with correct endpoint, model, cost_tier=0, auth_type=none
- [ ] `aimee agent test local` returns success (model responds to a simple prompt)
- [ ] `aimee delegate draft "summarize this function"` routes to the local agent when it's the cheapest enabled agent
- [ ] The script detects available RAM and recommends an appropriate model size
- [ ] The script is idempotent: running it again updates rather than duplicates the agent entry
- [ ] llama.cpp server (llama-server on :8080) is detected as an alternative to Ollama
- [ ] Custom endpoint entry works for any OpenAI-compatible API (e.g., LM Studio, vLLM, text-generation-webui)

## Owner and Effort

- **Owner:** Aimee delegate
- **Effort:** M (medium) — the script is straightforward bash; the C code already supports local agents with no changes
- **Dependencies:** None — all required infrastructure (`aimee agent add`, `aimee agent test`, `auth_type: none` support) already exists

## Rollout and Rollback

- **Rollout:** New script added to repo root. `install.sh` gains an optional prompt. No forced changes to existing installs.
- **Rollback:** Remove `add-local-delegate.sh` and revert the `install.sh` prompt. Existing agent configs are untouched.
- **Blast radius:** Minimal. The local delegate is additive. If the local model is slow or broken, the fallback chain routes to the next agent. No existing functionality is modified.

## Test Plan

- [ ] Unit tests: None needed — no C code changes
- [ ] Integration tests: Add `test_local_delegate.sh` that:
  - Mocks `ollama` CLI with a stub that returns success
  - Runs `add-local-delegate.sh` in non-interactive mode (env vars for choices)
  - Verifies `agents.json` contains the expected agent entry
  - Verifies `aimee agent list --json` includes the local agent
- [ ] Failure injection:
  - Ollama installed but not running → script offers to start it
  - Model pull fails (network error) → script reports error and exits cleanly
  - `aimee agent test` fails → script reports failure with diagnostic suggestions
  - No runtime detected, user declines install → clean exit, no partial config
- [ ] Integration test: fresh environment with no Ollama — install flow works end-to-end (containerized test)
- [ ] Integration test: environment with Ollama running — detection and model pull succeeds
- [ ] Integration test: idempotent — running script twice updates agent, does not duplicate

## Operational Impact

- **Metrics:** None new — existing agent delegation metrics cover local agents.
- **Logging:** Existing `aimee delegate` logging applies. No new log lines needed.
- **Alerts:** None.
- **Disk/CPU/Memory:** Model files consume 1-6 GB disk (depending on choice). CPU inference uses significant CPU during generation — this is expected and the reason for the 300s timeout. RAM usage equals model size during inference. No impact when idle.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| `add-local-delegate.sh` script | P2 | M | High — unlocks free delegation for all users |
| `install.sh` integration | P2 | S | Medium — discoverability during first install |
| RAM-based model recommendations | P3 | S | Low — convenience, sensible defaults work without it |

## Trade-offs

### Alternatives considered

**1. Auto-install Ollama without asking.**
Rejected. Installing system software silently violates user trust. The script should detect and offer, not force.

**Supply-chain risk for Ollama install:** The script downloads from `https://ollama.com/install.sh`. To mitigate: (a) download to a temp file instead of piping to `sh`, (b) verify the file starts with a shebang (basic sanity), (c) use HTTPS (already). Full signature verification is not feasible because Ollama does not publish installer signatures. This is the same trust model as installing Ollama directly — the script merely automates what the user would do manually. The user is prompted before installation proceeds.

**2. Bundle a model binary in the repo.**
Rejected. Models are 1-6 GB. Bloats the repo and goes stale quickly. Better to pull from Ollama's registry at setup time.

**3. Support GPU detection and GPU-optimized models.**
Deferred. GPU setup (CUDA, ROCm, Metal) is complex and hardware-specific. CPU-only is the universally available baseline. GPU support can be a follow-up proposal — the script architecture (detect → select → pull → register) extends naturally.

**4. Use `aimee agent setup` instead of a separate script.**
The existing `aimee agent setup` flow is designed for cloud APIs with auth flows. Local models have fundamentally different concerns (runtime detection, model pulling, RAM sizing). A separate script keeps the cloud and local paths clean. However, `add-local-delegate.sh` still uses `aimee agent add` under the hood, so there's no code duplication.

**5. Support llama.cpp model file management (GGUF download, quantization selection).**
Deferred. Ollama abstracts this well. Users running llama-server directly are advanced enough to manage their own model files. The "custom endpoint" path covers this case.

### Known limitations

- **CPU inference is slow.** A 4B parameter model on CPU generates ~10-20 tokens/second. This is fine for short tasks (review, summarize, draft) but not for long agentic loops. The 300s timeout and model size recommendations mitigate this.
- **No automatic Ollama startup.** If the user reboots and Ollama isn't configured as a service, the local delegate will fail until they run `ollama serve`. The fallback chain handles this gracefully (routes to next agent). A future improvement could add an `ollama serve` systemd unit.
- **Model recommendations will go stale.** New models release frequently. The recommended model list is hardcoded in the script. This is acceptable — the defaults work, and users can always enter a custom model name.
