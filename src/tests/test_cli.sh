#!/bin/bash
# test_cli.sh: smoke tests for the aimee CLI
# Run from the repo root: ./src/tests/test_cli.sh
# Commands are dispatched in-process via the server's cli.forward handler.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

AIMEE="$REPO_ROOT/aimee"
export HOME=$(mktemp -d /tmp/aimee-test-home-XXXXXX)
mkdir -p "$HOME/.config/aimee"

PASS=0
FAIL=0

require_binary() {
    if [ ! -x "$1" ]; then
        echo "missing test prerequisite: $1"
        exit 1
    fi
}

check() {
    local desc="$1"
    shift
    if "$@" >/dev/null 2>&1; then
        PASS=$((PASS + 1))
    else
        echo "FAIL: $desc"
        FAIL=$((FAIL + 1))
    fi
}

check_output() {
    local desc="$1"
    local expected="$2"
    shift 2
    local output
    output=$("$@" 2>&1) || true
    if echo "$output" | grep -qF "$expected"; then
        PASS=$((PASS + 1))
    else
        echo "FAIL: $desc (expected '$expected', got '$(echo "$output" | head -1)')"
        FAIL=$((FAIL + 1))
    fi
}

# --- Init (must run first) ---
require_binary "$AIMEE"
check "init" $AIMEE init

# --- Basic commands ---
check "version" $AIMEE version
check_output "version --json" "\"version\"" $AIMEE --json version

# --- Memory ---
check "memory store" $AIMEE memory store testkey "test content"
check_output "memory list json" "\"key\":\"testkey\"" $AIMEE --json memory list
check_output "memory read json" "\"context\"" $AIMEE --json memory read

# --- Rules ---
check_output "rules list json" "[]" $AIMEE --json rules list
check_output "rules generate json" "\"content\"" $AIMEE --json rules generate
check "feedback +" $AIMEE + "Test positive feedback"
check "feedback -" $AIMEE - "Test negative feedback"
check "feedback --hard" $AIMEE + --hard "Never push to main"

# --- Index ---
check_output "index overview json" "[]" $AIMEE --json index overview

# --- Mode ---
check "mode get" $AIMEE mode
check "mode plan" $AIMEE plan
check "mode implement" $AIMEE implement

# --- Agent ---
check_output "agent list (empty)" "[]" $AIMEE --json agent list

# --- Plans ---
check_output "plans list" "ID" $AIMEE plans list

# --- Jobs ---
check_output "jobs list" "ID" $AIMEE jobs list

# --- Trace ---
check_output "trace list" "ID" $AIMEE trace list

# --- Eval results ---
check "eval results" $AIMEE eval results

# --- Env ---
check "env detect" $AIMEE env detect

# --- Contract ---
check "contract" $AIMEE contract

# --- Manifest ---
check "manifest list" $AIMEE manifest list

# --- Help ---
check_output "help (no args)" "Usage:" $AIMEE help
check_output "help memory" "Subcommands:" $AIMEE help memory
check_output "help agent" "Subcommands:" $AIMEE help agent
check_output "help index" "Subcommands:" $AIMEE help index
check_output "help version" "Print version" $AIMEE help version
check_output "--help" "Commands:" $AIMEE --help
check_output "memory (no subcommand)" "Subcommands:" $AIMEE memory
check_output "index (no subcommand)" "Subcommands:" $AIMEE index

# --- Cleanup ---
rm -rf "$HOME"

echo ""
echo "$((PASS + FAIL)) tests: $PASS passed, $FAIL failed."
[ "$FAIL" -eq 0 ] || exit 1
