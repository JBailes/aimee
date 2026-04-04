#!/usr/bin/env bash
set -euo pipefail

# add-local-delegate.sh
# Sets up a local AI delegate using Ollama, llama.cpp, or a custom endpoint.
# Can be run standalone or called from install.sh.

AIMEE_BIN="${AIMEE_BIN:-aimee}"

# Colors (if terminal supports them)
if [ -t 1 ]; then
    BOLD='\033[1m'
    GREEN='\033[0;32m'
    YELLOW='\033[0;33m'
    RED='\033[0;31m'
    RESET='\033[0m'
else
    BOLD='' GREEN='' YELLOW='' RED='' RESET=''
fi

info()  { echo -e "${GREEN}>${RESET} $*"; }
warn()  { echo -e "${YELLOW}!${RESET} $*"; }
error() { echo -e "${RED}x${RESET} $*"; }
bold()  { echo -e "${BOLD}$*${RESET}"; }

# --- Step 1: Detect runtime ---

ENDPOINT=""
RUNTIME=""

detect_runtime() {
    # 1. Check for Ollama
    if command -v ollama &>/dev/null; then
        if curl -sf http://localhost:11434/api/tags &>/dev/null; then
            ENDPOINT="http://localhost:11434/v1"
            RUNTIME="ollama"
            return 0
        else
            warn "Ollama is installed but not running."
            read -rp "  Start Ollama now? [Y/n] " start_answer
            if [[ ! "${start_answer:-Y}" =~ ^[Nn] ]]; then
                ollama serve &>/dev/null &
                sleep 2
                if curl -sf http://localhost:11434/api/tags &>/dev/null; then
                    ENDPOINT="http://localhost:11434/v1"
                    RUNTIME="ollama"
                    info "Ollama started"
                    return 0
                else
                    warn "Ollama failed to start"
                fi
            fi
        fi
    fi

    # 2. Check for llama-server (llama.cpp)
    if curl -sf http://localhost:8080/v1/models &>/dev/null; then
        ENDPOINT="http://localhost:8080/v1"
        RUNTIME="llama-cpp"
        return 0
    fi

    # 3. Nothing found
    RUNTIME="none"
    return 1
}

# --- Step 2: Handle missing runtime ---

handle_no_runtime() {
    echo ""
    echo "  No local inference runtime detected."
    echo ""
    echo "  1) Install Ollama (recommended)"
    echo "  2) Enter a custom OpenAI-compatible endpoint"
    echo "  3) Skip local delegate setup"
    echo ""
    read -rp "  Select [1-3]: " runtime_choice
    case "${runtime_choice:-3}" in
        1)
            info "Installing Ollama..."
            local installer
            installer=$(mktemp /tmp/ollama-install.XXXXXX.sh)
            if ! curl -fsSL https://ollama.com/install.sh -o "$installer"; then
                error "Failed to download Ollama installer"
                rm -f "$installer"
                return 1
            fi
            # Basic sanity check
            if ! head -1 "$installer" | grep -q '^#!'; then
                error "Downloaded file does not appear to be a shell script"
                rm -f "$installer"
                return 1
            fi
            chmod +x "$installer"
            bash "$installer"
            rm -f "$installer"
            ollama serve &>/dev/null &
            sleep 3
            if ! curl -sf http://localhost:11434/api/tags &>/dev/null; then
                error "Ollama installed but failed to start"
                return 1
            fi
            ENDPOINT="http://localhost:11434/v1"
            RUNTIME="ollama"
            info "Ollama installed and running"
            ;;
        2)
            read -rp "  Endpoint URL: " custom_endpoint
            if [ -z "$custom_endpoint" ]; then
                error "No endpoint provided"
                return 1
            fi
            ENDPOINT="$custom_endpoint"
            RUNTIME="custom"
            ;;
        3)
            info "Skipping local delegate setup"
            exit 0
            ;;
        *)
            warn "Invalid choice, skipping"
            exit 0
            ;;
    esac
}

# --- Step 3: Select model ---

MODEL=""
MODEL_LIST_8GB=("qwen3:8b" "qwen3:4b" "phi4-mini")
MODEL_LIST_4GB=("qwen3:4b" "llama3.2:3b" "qwen3:1.7b")
MODEL_LIST_LOW=("qwen3:1.7b" "gemma3:1b")

select_model() {
    local ram_gb
    ram_gb=$(awk '/MemTotal/ {printf "%.0f", $2/1024/1024}' /proc/meminfo 2>/dev/null || echo "4")

    echo ""
    bold "Select a model for CPU inference"
    echo "  System RAM: ${ram_gb} GB"
    echo ""

    local models=()
    local default_num=1

    if [ "$ram_gb" -ge 8 ]; then
        models=("${MODEL_LIST_8GB[@]}")
        echo "  1) qwen3:8b       (~5.5 GB, best quality)     [recommended]"
        echo "  2) qwen3:4b       (~3.0 GB, balanced)"
        echo "  3) phi4-mini      (~2.5 GB, strong reasoning)"
    elif [ "$ram_gb" -ge 4 ]; then
        models=("${MODEL_LIST_4GB[@]}")
        echo "  1) qwen3:4b       (~3.0 GB, balanced)         [recommended]"
        echo "  2) llama3.2:3b    (~2.5 GB, general purpose)"
        echo "  3) qwen3:1.7b     (~1.5 GB, fast)"
    else
        models=("${MODEL_LIST_LOW[@]}")
        echo "  1) qwen3:1.7b     (~1.5 GB, fast)             [recommended]"
        echo "  2) gemma3:1b      (~1.0 GB, minimal)"
    fi
    echo "  c) Custom (enter model name)"
    echo ""

    read -rp "  Select [1-${#models[@]}/c] (default: $default_num): " model_choice
    model_choice="${model_choice:-$default_num}"

    if [ "$model_choice" = "c" ] || [ "$model_choice" = "C" ]; then
        read -rp "  Model name: " MODEL
        if [ -z "$MODEL" ]; then
            error "No model name provided"
            return 1
        fi
    elif [[ "$model_choice" =~ ^[0-9]+$ ]] && [ "$model_choice" -ge 1 ] && [ "$model_choice" -le "${#models[@]}" ]; then
        MODEL="${models[$((model_choice - 1))]}"
    else
        warn "Invalid choice, using default"
        MODEL="${models[0]}"
    fi

    info "Selected model: $MODEL"
}

# --- Step 4: Pull model (Ollama only) ---

pull_model() {
    if [ "$RUNTIME" != "ollama" ]; then
        return 0
    fi

    # Check if model is already pulled
    if ollama list 2>/dev/null | grep -q "^${MODEL}"; then
        info "Model $MODEL already available"
        return 0
    fi

    info "Pulling model $MODEL (this may take a few minutes)..."
    if ! ollama pull "$MODEL"; then
        error "Failed to pull model $MODEL"
        echo "  Check your internet connection and try: ollama pull $MODEL"
        return 1
    fi
    info "Model $MODEL ready"
}

# --- Step 5: Register agent ---

AGENT_NAME="local"

register_agent() {
    # Check if agent already exists — update it instead of duplicating
    local existing
    existing=$("$AIMEE_BIN" agent list --json 2>/dev/null | \
        python3 -c "import json,sys; agents=json.load(sys.stdin); print('yes' if any(a['name']=='$AGENT_NAME' for a in agents) else 'no')" 2>/dev/null || echo "no")

    if [ "$existing" = "yes" ]; then
        info "Updating existing agent '$AGENT_NAME'"
        "$AIMEE_BIN" agent remove "$AGENT_NAME" 2>/dev/null || true
    fi

    "$AIMEE_BIN" agent add "$AGENT_NAME" "$ENDPOINT" "$MODEL" \
        --auth-type none \
        --roles code,review,draft,explain,refactor,summarize \
        --cost-tier 0 \
        --tools \
        --timeout 300000

    info "Agent '$AGENT_NAME' registered: $MODEL @ $ENDPOINT"
}

# --- Step 6: Update fallback chain ---

update_fallback_chain() {
    local config_path
    config_path="$HOME/.config/aimee/agents.json"

    if [ ! -f "$config_path" ]; then
        return 0
    fi

    python3 -c "
import json

with open('$config_path') as f:
    cfg = json.load(f)

chain = cfg.get('fallback_chain', [])
if '$AGENT_NAME' not in chain:
    chain.insert(0, '$AGENT_NAME')
    cfg['fallback_chain'] = chain
    with open('$config_path', 'w') as f:
        json.dump(cfg, f, indent=2)
        f.write('\n')
" 2>/dev/null || true
}

# --- Step 7: Health check ---

health_check() {
    info "Testing agent..."
    if "$AIMEE_BIN" agent test "$AGENT_NAME" 2>/dev/null; then
        info "Agent '$AGENT_NAME' is working"
        return 0
    else
        warn "Agent test failed. The model may still be loading."
        echo "  Try again in a moment: aimee agent test $AGENT_NAME"
        return 1
    fi
}

# --- Main ---

main() {
    echo ""
    bold "Local AI Delegate Setup"
    echo ""

    # Detect runtime
    if ! detect_runtime; then
        handle_no_runtime
    else
        info "Detected runtime: $RUNTIME ($ENDPOINT)"
    fi

    # Select and pull model
    if [ "$RUNTIME" != "custom" ] || [ -z "$MODEL" ]; then
        select_model
    fi

    pull_model

    # Register and test
    register_agent
    update_fallback_chain
    health_check

    echo ""
    bold "Local delegate ready."
    echo "  Use: aimee delegate draft \"your task here\""
    echo "  The local model ($MODEL) will be tried first for delegation."
    echo ""
}

main "$@"
