#!/usr/bin/env bash
set -euo pipefail

# aimee hook configurator
# Detects and configures hooks for AI coding tools.
# Called by install.sh and update.sh.

INSTALL_DIR="/usr/local/bin"
AIMEE_BIN="$INSTALL_DIR/aimee"
HOME_DIR="$HOME"
CONFIGURED=0

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

configure_json_hooks() {
    local name="$1"
    local config_path="$2"
    local pre_event="$3"
    local post_event="$4"
    local session_event="$5"
    local pre_matcher="$6"
    local post_matcher="$7"
    local mcp_path="${8:-$config_path}"

    local settings="{}"
    if [ -f "$config_path" ]; then
        settings=$(cat "$config_path")
    fi

    local mcp_settings="{}"
    if [ "$mcp_path" != "$config_path" ] && [ -f "$mcp_path" ]; then
        mcp_settings=$(cat "$mcp_path")
    fi

    local new_settings
    new_settings=$(python3 -c "
import json, sys

settings = json.loads('''$settings''')
mcp_settings = json.loads('''$mcp_settings''') if '$mcp_path' != '$config_path' else settings

hooks = settings.get('hooks', {})
mcp = mcp_settings.get('mcpServers', {})
bin_path = '$AIMEE_BIN'
mcp_bin = bin_path + '-mcp'

def add_hook(event, matcher, command):
    entries = hooks.get(event, [])
    # Remove existing aimee hooks for this event to avoid duplicates/stale paths
    entries = [e for e in entries
               if not any('aimee' in h.get('command', '')
                          for h in e.get('hooks', []))]
    entries.append({
        'matcher': matcher,
        'hooks': [{'type': 'command', 'command': command}]
    })
    hooks[event] = entries

if '$session_event' != 'NONE':
    add_hook('$session_event', 'startup|resume|compact', bin_path + ' session-start')
if '$pre_event' != 'NONE':
    add_hook('$pre_event', '$pre_matcher', bin_path + ' hooks pre')
if '$post_event' != 'NONE':
    add_hook('$post_event', '$post_matcher', bin_path + ' hooks post')

settings['hooks'] = hooks

# Add Aimee MCP server
mcp['aimee'] = {'command': mcp_bin}
if '$mcp_path' == '$config_path':
    settings['mcpServers'] = mcp
else:
    mcp_settings['mcpServers'] = mcp
    print('---MCP---')
    print(json.dumps(mcp_settings, indent=2))
    print('---CONFIG---')

print(json.dumps(settings, indent=2))
")

    mkdir -p "$(dirname "$config_path")"
    if echo "$new_settings" | grep -qF -- "---MCP---"; then
        echo "$new_settings" | sed -n '/---MCP---/,/---CONFIG---/p' | sed '1d;$d' > "$mcp_path"
        echo "$new_settings" | sed -n '/---CONFIG---/,$p' | sed '1d' > "$config_path"
    else
        echo "$new_settings" > "$config_path"
    fi
    info "Configured $name: $config_path"
    if [ "$mcp_path" != "$config_path" ]; then
        info "Configured $name MCP: $mcp_path"
    fi
    CONFIGURED=$((CONFIGURED + 1))
}

# --- Detect and configure AI coding tools ---

# Claude Code
if [ -d "$HOME_DIR/.claude" ] || command -v claude &>/dev/null; then
    CLAUDE_SETTINGS="$HOME_DIR/.claude/settings.json"
    configure_json_hooks "Claude Code" \
        "$CLAUDE_SETTINGS" \
        "PreToolUse" "PostToolUse" "SessionStart" \
        "Edit|Write|MultiEdit|Bash|Read|Glob|Grep" "Edit|Write|MultiEdit"

    # Ensure Gemini is in the auto-allow list if permissions are already set
    python3 -c "
import json
import os
import sys

config_path = '$CLAUDE_SETTINGS'
if not os.path.exists(config_path):
    sys.exit(0)

with open(config_path) as f:
    settings = json.load(f)

if 'permissions' in settings and 'allow' in settings['permissions']:
    allow = settings['permissions']['allow']
    if 'Gemini(gemini:*)' not in allow:
        # Insert after Bash(git:*) or at the end
        try:
            idx = allow.index('Bash(git:*)')
            allow.insert(idx + 1, 'Gemini(gemini:*)')
        except ValueError:
            allow.append('Gemini(gemini:*)')
        
        with open(config_path, 'w') as f:
            json.dump(settings, f, indent=2)
            f.write('\n')
" 2>/dev/null
fi

# Claude Desktop (MCP only, no hooks)
CLAUDE_DESKTOP_CONFIG=""
if [[ "$OSTYPE" == "darwin"* ]]; then
    CLAUDE_DESKTOP_CONFIG="$HOME_DIR/Library/Application Support/Claude/claude_desktop_config.json"
elif [[ "$OSTYPE" == "linux-gnu"* ]]; then
    CLAUDE_DESKTOP_CONFIG="$HOME_DIR/.config/Claude/claude_desktop_config.json"
fi

if [ -n "$CLAUDE_DESKTOP_CONFIG" ] && [ -d "$(dirname "$CLAUDE_DESKTOP_CONFIG")" ]; then
    configure_json_hooks "Claude Desktop" \
        "$CLAUDE_DESKTOP_CONFIG" \
        "NONE" "NONE" "NONE" "" ""
fi

# Gemini CLI
if [ -d "$HOME_DIR/.gemini" ] || command -v gemini &>/dev/null; then
    configure_json_hooks "Gemini CLI" \
        "$HOME_DIR/.gemini/settings.json" \
        "BeforeTool" "AfterTool" "SessionStart" \
        "write_file|replace|shell" "write_file|replace"
fi

# Codex CLI
if [ -d "$HOME_DIR/.codex" ] || command -v codex &>/dev/null; then
    configure_json_hooks "Codex CLI" \
        "$HOME_DIR/.codex/hooks.json" \
        "PreToolUse" "PostToolUse" "SessionStart" \
        "Bash" "Bash" \
        "$HOME_DIR/.codex/mcp-config.json"
fi

# GitHub Copilot
if [ -d "$HOME_DIR/.copilot" ] || command -v copilot &>/dev/null; then
    COPILOT_CONFIG="$HOME_DIR/.copilot/config.json"
    # GitHub Copilot CLI uses hooks in config.json and MCP in mcp-config.json
    configure_json_hooks "GitHub Copilot" \
        "$COPILOT_CONFIG" \
        "PreToolUse" "PostToolUse" "SessionStart" \
        "Bash|Edit|Write" "Edit|Write" \
        "$HOME_DIR/.copilot/mcp-config.json"
fi

if [ "$CONFIGURED" -gt 0 ]; then
    info "aimee hooks refreshed for $CONFIGURED tool(s)."
fi
