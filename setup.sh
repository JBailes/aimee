#!/usr/bin/env bash
set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

sudo apt-get update -qq
sudo apt-get install -qq -y gcc make libsqlite3-dev libcurl4-openssl-dev git universal-ctags

cd "$SCRIPT_DIR/src" && make
sudo ln -sf "$SCRIPT_DIR/aimee" /usr/local/bin/aimee

aimee init
aimee setup

echo "aimee bootstrap complete."
