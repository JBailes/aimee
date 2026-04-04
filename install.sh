#!/usr/bin/env bash
set -euo pipefail

# aimee installer
# Builds the C binary, installs it, creates the database, and configures hooks.

INSTALL_DIR="/usr/local/bin"
AIMEE_BIN="$INSTALL_DIR/aimee"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$SCRIPT_DIR/src"
LOCAL_BIN="$SCRIPT_DIR/aimee"
LOCAL_SERVER="$SCRIPT_DIR/aimee-server"

# Colors (if terminal supports them)
if [ -t 1 ]; then
    BOLD='\033[1m'
    GREEN='\033[0;32m'
    YELLOW='\033[0;33m'
    RESET='\033[0m'
else
    BOLD='' GREEN='' YELLOW='' RESET=''
fi

info()  { echo -e "${GREEN}>${RESET} $*"; }
warn()  { echo -e "${YELLOW}!${RESET} $*"; }
bold()  { echo -e "${BOLD}$*${RESET}"; }

# --- Prerequisites ---

if ! command -v gcc &>/dev/null; then
    echo "Error: gcc is not installed. Install build-essential."
    exit 1
fi

if ! pkg-config --exists sqlite3 2>/dev/null; then
    echo "Error: libsqlite3-dev is not installed."
    exit 1
fi

# Check for libcurl
if ! pkg-config --exists libcurl 2>/dev/null; then
    echo "Error: libcurl-dev is not installed."
    echo "  Debian/Ubuntu: apt-get install libcurl4-openssl-dev"
    echo "  macOS: brew install curl"
    exit 1
fi

# Install universal-ctags for code indexing (if missing)
if ! command -v ctags &>/dev/null; then
    info "Installing universal-ctags..."
    sudo apt-get install -y universal-ctags >/dev/null 2>&1
fi

# Verify FTS5 support in system SQLite (install sqlite3 CLI if missing)
if ! command -v sqlite3 &>/dev/null; then
    info "Installing sqlite3..."
    sudo apt-get install -y sqlite3 >/dev/null 2>&1
fi

if ! sqlite3 :memory: "PRAGMA compile_options" 2>/dev/null | grep -q ENABLE_FTS5; then
    echo "Error: system SQLite does not have FTS5 enabled."
    echo "Install a SQLite build with FTS5 support (default on Debian 13+)."
    exit 1
fi

# --- Build (skip if binary is newer than all source) ---

needs_build() {
    if [ ! -f "$LOCAL_BIN" ] || [ ! -f "$LOCAL_SERVER" ]; then
        return 0
    fi
    while IFS= read -r -d '' src; do
        if [ "$src" -nt "$LOCAL_BIN" ] || [ "$src" -nt "$LOCAL_SERVER" ]; then
            return 0
        fi
    done < <(find "$SRC_DIR" -name '*.c' -o -name '*.h' | tr '\n' '\0')
    if [ "$SRC_DIR/Makefile" -nt "$LOCAL_BIN" ]; then
        return 0
    fi
    return 1
}

if needs_build; then
    info "Building aimee..."
    cd "$SRC_DIR"
    make all server
    cd "$SCRIPT_DIR"
else
    info "Binaries are up to date, skipping build"
fi

# --- Stop running server before overwriting binaries ---

if pgrep -x aimee-server >/dev/null 2>&1; then
    info "Stopping aimee-server before install..."
    pkill -x aimee-server 2>/dev/null || true
    # Wait up to 3 seconds for graceful shutdown, then force kill
    for i in 1 2 3; do
        pgrep -x aimee-server >/dev/null 2>&1 || break
        sleep 1
    done
    if pgrep -x aimee-server >/dev/null 2>&1; then
        warn "aimee-server did not exit gracefully, sending SIGKILL..."
        pkill -9 -x aimee-server 2>/dev/null || true
        sleep 1
    fi
fi

# --- Install binaries ---

install_bin() {
    local src="$1" dst="$2"
    if [ -w "$INSTALL_DIR" ]; then
        rm -f "$dst"
        cp "$src" "$dst"
    else
        sudo rm -f "$dst"
        sudo cp "$src" "$dst"
    fi
    chmod +x "$dst"
}

if [ ! -w "$INSTALL_DIR" ]; then
    info "Installing to $INSTALL_DIR (requires sudo)..."
fi

install_bin "$LOCAL_BIN" "$INSTALL_DIR/aimee"
install_bin "$LOCAL_SERVER" "$INSTALL_DIR/aimee-server"

# Remove legacy binaries (aimee-mcp merged into aimee mcp-serve, aimem renamed to aimee)
for legacy in "$INSTALL_DIR/aimee-mcp" "$INSTALL_DIR/aimem"; do
    if [ -f "$legacy" ]; then
        info "Removing legacy binary: $legacy"
        rm -f "$legacy" 2>/dev/null || sudo rm -f "$legacy" 2>/dev/null || true
    fi
done

info "Installed: $INSTALL_DIR/aimee, $INSTALL_DIR/aimee-server"

# --- Initialize database ---

"$AIMEE_BIN" init --quiet 2>/dev/null || "$AIMEE_BIN" version
info "Database initialized"
info "Client integrations refreshed"

# --- Choose primary AI CLI ---

CONFIG_DIR="$HOME/.config/aimee"
CONFIG_FILE="$CONFIG_DIR/config.json"

# Read current provider from config (default: claude)
CURRENT_PROVIDER="claude"
if [ -f "$CONFIG_FILE" ]; then
    CURRENT_PROVIDER=$(python3 -c "
import json
with open('$CONFIG_FILE') as f:
    print(json.load(f).get('provider', 'claude'))
" 2>/dev/null || echo "claude")
fi

echo ""
bold "Choose your primary AI CLI"
echo "  This is the AI coding tool that launches when you run ${BOLD}aimee${RESET}."
echo ""
echo "  1) Claude   (claude)"
echo "  2) Codex    (codex)"
echo "  3) Gemini   (gemini)"
echo "  4) OpenAI-compatible (any OpenAI-compatible API)"
echo ""

# Map current provider to a number for the default prompt
case "$CURRENT_PROVIDER" in
    claude) DEFAULT_NUM=1 ;;
    codex)  DEFAULT_NUM=2 ;;
    gemini) DEFAULT_NUM=3 ;;
    openai) DEFAULT_NUM=4 ;;
    *)      DEFAULT_NUM=1 ;;
esac

read -rp "  Select [1-4] (current: $DEFAULT_NUM): " cli_choice
cli_choice="${cli_choice:-$DEFAULT_NUM}"

case "$cli_choice" in
    1) CHOSEN_PROVIDER="claude" ;;
    2) CHOSEN_PROVIDER="codex"  ;;
    3) CHOSEN_PROVIDER="gemini" ;;
    4) CHOSEN_PROVIDER="openai" ;;
    *)
        warn "Invalid choice. Keeping current provider: $CURRENT_PROVIDER"
        CHOSEN_PROVIDER="$CURRENT_PROVIDER"
        ;;
esac

# Save provider to config.json
mkdir -p "$CONFIG_DIR"
if [ -f "$CONFIG_FILE" ]; then
    python3 -c "
import json
with open('$CONFIG_FILE') as f:
    cfg = json.load(f)
cfg['provider'] = '$CHOSEN_PROVIDER'
with open('$CONFIG_FILE', 'w') as f:
    json.dump(cfg, f, indent=2)
    f.write('\n')
"
else
    echo "{\"provider\": \"$CHOSEN_PROVIDER\"}" > "$CONFIG_FILE"
    chmod 0600 "$CONFIG_FILE"
fi

info "Primary CLI set to: $CHOSEN_PROVIDER"

# For non-openai providers, ask: native CLI or built-in CLI?
USE_BUILTIN=0
if [ "$CHOSEN_PROVIDER" != "openai" ]; then
    echo ""
    bold "How should aimee launch $CHOSEN_PROVIDER?"
    echo "  a) Use the $CHOSEN_PROVIDER CLI directly (requires $CHOSEN_PROVIDER to be installed)"
    echo "  b) Use Aimee's built-in chat (routes through the $CHOSEN_PROVIDER API)"
    echo ""
    read -rp "  Select [a/b] (default: a): " cli_mode
    cli_mode="${cli_mode:-a}"

    if [ "$cli_mode" = "b" ] || [ "$cli_mode" = "B" ]; then
        USE_BUILTIN=1
    fi
fi

if [ "$CHOSEN_PROVIDER" != "openai" ] && [ "$USE_BUILTIN" = "0" ]; then
    # Native CLI mode: check that it's installed
    if ! command -v "$CHOSEN_PROVIDER" &>/dev/null; then
        warn "$CHOSEN_PROVIDER is not installed. You will need to install it before running aimee."
    fi

    # Ensure use_builtin_cli is false
    python3 -c "
import json
with open('$CONFIG_FILE') as f:
    cfg = json.load(f)
cfg['use_builtin_cli'] = False
with open('$CONFIG_FILE', 'w') as f:
    json.dump(cfg, f, indent=2)
    f.write('\n')
"
fi

# Built-in CLI configuration (for openai or use_builtin_cli)
if [ "$CHOSEN_PROVIDER" = "openai" ] || [ "$USE_BUILTIN" = "1" ]; then
    # Determine provider-specific defaults
    case "$CHOSEN_PROVIDER" in
        claude)
            DEFAULT_ENDPOINT="https://api.anthropic.com/v1"
            DEFAULT_MODEL="claude-sonnet-4-20250514"
            KEY_ENV_HINT="ANTHROPIC_API_KEY"
            KEY_FILE_NAME="claude.key"
            ;;
        gemini)
            DEFAULT_ENDPOINT="https://generativelanguage.googleapis.com/v1beta/openai"
            DEFAULT_MODEL="gemini-2.5-pro"
            KEY_ENV_HINT="GEMINI_API_KEY"
            KEY_FILE_NAME="gemini.key"
            ;;
        codex)
            DEFAULT_ENDPOINT="https://api.openai.com/v1"
            DEFAULT_MODEL="o3"
            KEY_ENV_HINT="OPENAI_API_KEY"
            KEY_FILE_NAME="openai.key"
            ;;
        *)
            DEFAULT_ENDPOINT="https://api.openai.com/v1"
            DEFAULT_MODEL="gpt-4o"
            KEY_ENV_HINT="OPENAI_API_KEY"
            KEY_FILE_NAME="openai.key"
            ;;
    esac

    echo ""
    bold "Configure ${CHOSEN_PROVIDER} API"

    # Read current values
    CURRENT_ENDPOINT="$DEFAULT_ENDPOINT"
    CURRENT_MODEL="$DEFAULT_MODEL"
    if [ -f "$CONFIG_FILE" ]; then
        SAVED_ENDPOINT=$(python3 -c "
import json
with open('$CONFIG_FILE') as f:
    print(json.load(f).get('openai_endpoint', ''))
" 2>/dev/null)
        SAVED_MODEL=$(python3 -c "
import json
with open('$CONFIG_FILE') as f:
    print(json.load(f).get('openai_model', ''))
" 2>/dev/null)
        # Use saved values if they're not the old defaults
        if [ -n "$SAVED_ENDPOINT" ] && [ "$SAVED_ENDPOINT" != "https://api.openai.com/v1" ]; then
            CURRENT_ENDPOINT="$SAVED_ENDPOINT"
        fi
        if [ -n "$SAVED_MODEL" ] && [ "$SAVED_MODEL" != "gpt-4o" ]; then
            CURRENT_MODEL="$SAVED_MODEL"
        fi
    fi

    echo ""
    read -rp "  API endpoint [$CURRENT_ENDPOINT]: " openai_endpoint
    openai_endpoint="${openai_endpoint:-$CURRENT_ENDPOINT}"

    read -rp "  Model [$CURRENT_MODEL]: " openai_model
    openai_model="${openai_model:-$CURRENT_MODEL}"

    echo ""
    echo "  Enter your API key (leave blank to keep existing or use $KEY_ENV_HINT env var):"
    read -rsp "  API key: " api_key
    echo ""

    OPENAI_KEY_CMD=""
    if [ -n "$api_key" ]; then
        KEY_FILE="$CONFIG_DIR/$KEY_FILE_NAME"
        echo -n "$api_key" > "$KEY_FILE"
        chmod 0600 "$KEY_FILE"
        OPENAI_KEY_CMD="cat $KEY_FILE"
        info "API key saved to $KEY_FILE"
    elif [ -f "$CONFIG_DIR/$KEY_FILE_NAME" ]; then
        OPENAI_KEY_CMD="cat $CONFIG_DIR/$KEY_FILE_NAME"
    fi

    python3 -c "
import json
with open('$CONFIG_FILE') as f:
    cfg = json.load(f)
cfg['use_builtin_cli'] = True
cfg['openai_endpoint'] = '$openai_endpoint'
cfg['openai_model'] = '$openai_model'
key_cmd = '$OPENAI_KEY_CMD'
if key_cmd:
    cfg['openai_key_cmd'] = key_cmd
with open('$CONFIG_FILE', 'w') as f:
    json.dump(cfg, f, indent=2)
    f.write('\n')
"
    info "Built-in CLI configured: $openai_model @ $openai_endpoint"
fi

# --- Optional: Local AI delegate ---

echo ""
bold "Set up a local AI delegate? (CPU-only, free)"
echo "  Offloads coding tasks (review, refactor, explain, draft)"
echo "  to a local model. Saves API costs and works offline."
echo ""
read -rp "  Set up local delegate? [y/N] " local_answer
if [[ "$local_answer" =~ ^[Yy] ]]; then
    AIMEE_BIN="$AIMEE_BIN" bash "$SCRIPT_DIR/add-local-delegate.sh" || warn "Local delegate setup failed (non-fatal)"
fi

# --- Detect and configure AI coding tools ---

bash "$SCRIPT_DIR/configure-hooks.sh"

# --- Summary ---

echo ""
if [ "$CONFIGURED" -eq 0 ]; then
    warn "No AI coding tools detected (checked for Claude Code, Gemini CLI, Codex CLI)."
    warn "Install one of them, then re-run this script."
else
    bold "aimee installed and configured for $CONFIGURED tool(s)."
    echo "Start a new session in your AI coding tool to activate."
fi
