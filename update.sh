#!/usr/bin/env bash
set -euo pipefail

# aimee updater
# Pulls latest source from GitHub and rebuilds if any source files changed.

INSTALL_DIR="/usr/local/bin"
AIMEE_BIN="$INSTALL_DIR/aimee"
AIMEE_SERVER_BIN="$INSTALL_DIR/aimee-server"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$SCRIPT_DIR/src"
LOCAL_BIN="$SCRIPT_DIR/aimee"
LOCAL_SERVER="$SCRIPT_DIR/aimee-server"

# Colors (if terminal supports them)
if [ -t 1 ]; then
    GREEN='\033[0;32m'
    YELLOW='\033[0;33m'
    RESET='\033[0m'
else
    GREEN='' YELLOW='' RESET=''
fi

info() { echo -e "${GREEN}>${RESET} $*"; }
warn() { echo -e "${YELLOW}!${RESET} $*"; }

# --- Pull latest ---

cd "$SCRIPT_DIR"
OLD_HEAD=$(git rev-parse HEAD)

info "Pulling latest from origin/main..."
git fetch origin main
git checkout main
git merge origin/main

NEW_HEAD=$(git rev-parse HEAD)

# --- Check if rebuild is needed ---

needs_build() {
    # No binary at all
    if [ ! -f "$LOCAL_BIN" ] || [ ! -f "$LOCAL_SERVER" ]; then
        return 0
    fi

    # HEAD moved and source files were in the diff
    if [ "$OLD_HEAD" != "$NEW_HEAD" ]; then
        if git diff --name-only "$OLD_HEAD" "$NEW_HEAD" -- src/ | grep -qE '\.(c|h)$|Makefile'; then
            return 0
        fi
    fi

    # Any source file newer than the binary
    while IFS= read -r -d '' src; do
        if [ "$src" -nt "$LOCAL_BIN" ]; then
            return 0
        fi
    done < <(find "$SRC_DIR" \( -name '*.c' -o -name '*.h' \) -print0)

    # Makefile itself changed
    if [ "$SRC_DIR/Makefile" -nt "$LOCAL_BIN" ]; then
        return 0
    fi

    return 1
}

needs_install() {
    if [ ! -f "$AIMEE_BIN" ] || [ ! -f "$AIMEE_SERVER_BIN" ]; then
        return 0
    fi
    return 1
}

# Ensure universal-ctags is installed for code indexing
if ! command -v ctags &>/dev/null; then
    info "Installing universal-ctags..."
    sudo apt-get install -y universal-ctags >/dev/null 2>&1
fi

if ! needs_build && ! needs_install; then
    info "Already up to date (binary is newer than all source files)"
    exit 0
fi

# --- Build ---

if needs_build; then
    info "Source files changed, rebuilding..."
    cd "$SRC_DIR"
    make
    make server
    cd "$SCRIPT_DIR"
else
    info "Local binaries are current; refreshing installed binaries"
fi

# --- Stop server if running (must happen before install to avoid "Text file busy") ---

SERVER_WAS_RUNNING=false
if pgrep -x aimee-server >/dev/null 2>&1; then
    info "Stopping aimee-server..."
    pkill -x aimee-server || true
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
    SERVER_WAS_RUNNING=true
fi

# --- Clean up legacy aimee-mcp binary (merged into aimee mcp-serve) ---

LEGACY_MCP="$INSTALL_DIR/aimee-mcp"
if [ -f "$LEGACY_MCP" ]; then
    info "Removing legacy aimee-mcp binary..."
    if [ -w "$INSTALL_DIR" ]; then
        rm -f "$LEGACY_MCP"
    else
        sudo rm -f "$LEGACY_MCP"
    fi
fi

# --- Install binary ---

if [ -w "$INSTALL_DIR" ]; then
    rm -f "$AIMEE_BIN" "$AIMEE_SERVER_BIN"
    cp "$LOCAL_BIN" "$AIMEE_BIN"
    cp "$LOCAL_SERVER" "$AIMEE_SERVER_BIN"
else
    info "Installing to $INSTALL_DIR (requires sudo)..."
    sudo rm -f "$AIMEE_BIN" "$AIMEE_SERVER_BIN"
    sudo cp "$LOCAL_BIN" "$AIMEE_BIN"
    sudo cp "$LOCAL_SERVER" "$AIMEE_SERVER_BIN"
fi
chmod +x "$AIMEE_BIN" "$AIMEE_SERVER_BIN"

# --- Restart server if it was running ---

if $SERVER_WAS_RUNNING; then
    info "Restarting aimee-server..."
    "$AIMEE_SERVER_BIN" 2>/dev/null &
    disown
    sleep 1
    info "aimee-server restarted"
fi

"$AIMEE_BIN" init --quiet >/dev/null 2>&1 || true

bash "$SCRIPT_DIR/configure-hooks.sh"

info "Updated: $AIMEE_BIN, $AIMEE_SERVER_BIN"
