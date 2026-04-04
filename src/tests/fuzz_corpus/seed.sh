#!/usr/bin/env bash
set -euo pipefail

DB_HOST="${DB_HOST:-localhost}"
DB_PORT="${DB_PORT:-5432}"

check_deps() {
    command -v psql &>/dev/null || { echo "psql not found"; exit 1; }
}

start_server() {
    local port="$1"
    echo "Starting on port $port"
    exec ./server --port "$port"
}

cleanup() {
    rm -f /tmp/app.pid
}

trap cleanup EXIT
check_deps
start_server "$DB_PORT"
