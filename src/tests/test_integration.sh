#!/bin/bash
# test_integration.sh: smoke tests for the aimee server architecture
# Run from the repo root: ./src/tests/test_integration.sh
#
# Tests the full client -> server -> response path including:
# - Server lifecycle (start, health, shutdown)
# - Authentication and capabilities
# - Session management
# - Memory CRUD via server
# - Hooks through server
# - CLI forwarding (pure client -> server -> in-process dispatch)
# - Tool execution via compute pool
# - Graceful degradation (no server)

set -e

# Resolve paths relative to repo root
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

AIMEE="$REPO_ROOT/aimee"
AIMEE_SERVER="$REPO_ROOT/aimee-server"
export PATH="$REPO_ROOT:$PATH"
export HOME=$(mktemp -d /tmp/aimee-integ-XXXXXX)
mkdir -p "$HOME/.config/aimee"
SOCKET="$HOME/.config/aimee/aimee.sock"

# Isolate from any running session: prevent connecting to a production server
unset AIMEE_SOCK

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

check_exit() {
    local desc="$1"
    local expected_code="$2"
    shift 2
    "$@" >/dev/null 2>&1
    local rc=$?
    if [ "$rc" -eq "$expected_code" ]; then
        PASS=$((PASS + 1))
    else
        echo "FAIL: $desc (expected exit $expected_code, got $rc)"
        FAIL=$((FAIL + 1))
    fi
}

require_binary "$AIMEE"
require_binary "$AIMEE_SERVER"

# Helper: send raw JSON to server socket, return response
srv_req() {
    echo "$1" | python3 -c "
import socket, json, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect('$SOCKET')
s.settimeout(10)
s.sendall((sys.stdin.read().strip() + '\n').encode())
buf = b''
while b'\n' not in buf:
    chunk = s.recv(4096)
    if not chunk: break
    buf += chunk
s.close()
for line in buf.decode().strip().split('\n'):
    if line.strip():
        print(line.strip())
        break
" 2>/dev/null
}

# Helper: send raw JSON, authenticate first, return response
srv_auth_req() {
    local token
    token=$(cat "$HOME/.config/aimee/server.token" 2>/dev/null | tr -d '\n')
    python3 -c "
import socket, json, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect('$SOCKET')
s.settimeout(10)
# auth
s.sendall((json.dumps({'method':'auth','token':'$token'}) + '\n').encode())
buf = b''
while b'\n' not in buf:
    chunk = s.recv(4096)
    if not chunk: break
    buf += chunk
buf = b''
# actual request
s.sendall(('$1' + '\n').encode())
while b'\n' not in buf:
    chunk = s.recv(4096)
    if not chunk: break
    buf += chunk
s.close()
for line in buf.decode().strip().split('\n'):
    if line.strip():
        print(line.strip())
        break
" 2>/dev/null
}

cleanup() {
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$HOME"
}
trap cleanup EXIT

# ============================================================
# Setup: initialize DB
# ============================================================

# Init DB via server (auto-start will create it)
check "init DB" $AIMEE init

# Kill any auto-started server so we start fresh
pkill -f "aimee-server.*$SOCKET" 2>/dev/null || true
sleep 1

# ============================================================
# 1. Auto-start (no server running)
# ============================================================

check_output "no server: version works" "aimee" $AIMEE version

# Disable auto-start for all server tests (they manage the server explicitly)
export AIMEE_NO_AUTOSTART=1
unset AIMEE_SOCK

check_output "no server: error without autostart" "cannot start" \
    $AIMEE memory list

# ============================================================
# 2. Server lifecycle
# ============================================================

$AIMEE_SERVER --foreground >/dev/null 2>&1 &
SERVER_PID=$!
sleep 1

check "server started" test -S "$SOCKET"

RESP=$(srv_req '{"method":"server.info"}')
check_output "server.info status" '"status":"ok"' echo "$RESP"
check_output "server.info version" '"server_version"' echo "$RESP"

RESP=$(srv_req '{"method":"server.health"}')
check_output "server.health status" '"status":"ok"' echo "$RESP"
check_output "server.health uptime" '"uptime"' echo "$RESP"

# ============================================================
# 3. Authentication
# ============================================================

# Unattested: read methods work
RESP=$(srv_req '{"method":"memory.list","limit":1}')
check_output "unattested: memory.list ok" '"status":"ok"' echo "$RESP"

RESP=$(srv_req '{"method":"rules.list"}')
check_output "unattested: rules.list ok" '"status":"ok"' echo "$RESP"

# Unattested: write methods blocked
RESP=$(srv_req '{"method":"tool.execute","tool":"bash","arguments":"{\"command\":\"echo hi\"}","session_id":"t","cwd":"/tmp","timeout_ms":1000}')
check_output "unattested: tool.execute blocked" "forbidden" echo "$RESP"

RESP=$(srv_req '{"method":"delegate","role":"test","prompt":"hi"}')
check_output "unattested: delegate blocked" "forbidden" echo "$RESP"

# Bad token
RESP=$(srv_req '{"method":"auth","token":"wrong-token-value"}')
check_output "bad token rejected" '"invalid token"' echo "$RESP"

# Good token
TOKEN=$(cat "$HOME/.config/aimee/server.token" | tr -d '\n')
RESP=$(srv_req "{\"method\":\"auth\",\"token\":\"$TOKEN\"}")
check_output "good token accepted" '"local_attested"' echo "$RESP"

# ============================================================
# 4. Session management
# ============================================================

RESP=$(srv_auth_req '{"method":"session.create","client_type":"test"}')
check_output "session.create" '"status":"ok"' echo "$RESP"
SID=$(echo "$RESP" | python3 -c "import sys,json; print(json.load(sys.stdin)['session_id'])" 2>/dev/null)

RESP=$(srv_auth_req '{"method":"session.list"}')
check_output "session.list has session" "$SID" echo "$RESP"

RESP=$(srv_auth_req "{\"method\":\"session.get\",\"session_id\":\"$SID\"}")
check_output "session.get" '"client_type":"test"' echo "$RESP"

RESP=$(srv_auth_req "{\"method\":\"session.close\",\"session_id\":\"$SID\"}")
check_output "session.close" '"status":"ok"' echo "$RESP"

RESP=$(srv_auth_req '{"method":"session.list"}')
check_output "session.list empty after close" '"sessions":[]' echo "$RESP"

# ============================================================
# 5. Memory via server
# ============================================================

RESP=$(srv_auth_req '{"method":"memory.store","key":"integ-test","content":"integration test value","tier":"L0","kind":"fact"}')
check_output "memory.store" '"status":"ok"' echo "$RESP"
MEM_ID=$(echo "$RESP" | python3 -c "import sys,json; print(int(json.load(sys.stdin)['id']))" 2>/dev/null)

RESP=$(srv_auth_req '{"method":"memory.list","tier":"L0","limit":10}')
check_output "memory.list has stored entry" "integ-test" echo "$RESP"

RESP=$(srv_auth_req "{\"method\":\"memory.get\",\"id\":$MEM_ID}")
check_output "memory.get by ID" "integration test value" echo "$RESP"

# ============================================================
# 6. Hooks through server
# ============================================================

RESP=$(echo '{"tool_name":"Read","tool_input":"{\"file_path\":\"main.c\"}"}' | CLAUDE_SESSION_ID=integ-test $AIMEE hooks pre 2>&1)
check_exit "hooks pre safe file" 0 echo '{"tool_name":"Read","tool_input":"{\"file_path\":\"main.c\"}"}' | CLAUDE_SESSION_ID=integ-test $AIMEE hooks pre

# ============================================================
# 7. CLI forwarding
# ============================================================

check_output "cli.forward: version" "aimee" $AIMEE version
check_output "cli.forward: env" "gcc" $AIMEE env

# ============================================================
# 8. Tool execution via compute pool
# ============================================================

TOKEN=$(cat "$HOME/.config/aimee/server.token" | tr -d '\n')
RESP=$(python3 -c "
import socket, json
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect('$SOCKET')
s.settimeout(10)
s.sendall((json.dumps({'method':'auth','token':'$TOKEN'})+'\n').encode())
s.recv(4096)
req = {'method':'tool.execute','tool':'bash','arguments':json.dumps({'command':'echo integ-ok'}),'session_id':'t','cwd':'/tmp','timeout_ms':5000}
s.sendall((json.dumps(req)+'\n').encode())
buf=b''
while b'\n' not in buf: buf+=s.recv(4096)
s.close()
print(buf.decode().strip().split('\n')[0])
" 2>/dev/null)
check_output "tool.execute bash" "integ-ok" echo "$RESP"

RESP=$(python3 -c "
import socket, json
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect('$SOCKET')
s.settimeout(10)
s.sendall((json.dumps({'method':'auth','token':'$TOKEN'})+'\n').encode())
s.recv(4096)
req = {'method':'tool.execute','tool':'read_file','arguments':json.dumps({'path':'/etc/hostname','limit':1}),'session_id':'t','cwd':'/tmp'}
s.sendall((json.dumps(req)+'\n').encode())
buf=b''
while b'\n' not in buf: buf+=s.recv(4096)
s.close()
print(buf.decode().strip().split('\n')[0])
" 2>/dev/null)
check_output "tool.execute read_file" '"status":"ok"' echo "$RESP"

# ============================================================
# 9. Server shutdown
# ============================================================

kill "$SERVER_PID" 2>/dev/null
wait "$SERVER_PID" 2>/dev/null || true
SERVER_PID=""

check "socket removed after shutdown" test ! -S "$SOCKET"

# ============================================================
# Results
# ============================================================

TOTAL=$((PASS + FAIL))
echo ""
echo "integration: $PASS/$TOTAL passed"
if [ "$FAIL" -gt 0 ]; then
    echo "FAILED ($FAIL failures)"
    exit 1
fi
echo "All integration tests passed."
