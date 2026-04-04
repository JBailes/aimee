#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

AIMEE="$REPO_ROOT/aimee"
AIMEE_SERVER="$REPO_ROOT/aimee-server"
export PATH="$REPO_ROOT:$PATH"

export HOME=$(mktemp -d /tmp/aimee-test-home-XXXXXX)
mkdir -p "$HOME/.config/aimee"
cd "$REPO_ROOT"

echo "Using AIMEE: $AIMEE"

# Init
$AIMEE init

# Create mock workspace manifest
echo "name: test-ws" > aimee.workspace.yaml

# 1. Test add-batch (new behavior)
mkdir -p docs/proposals/pending
echo "# Test Proposal" > docs/proposals/pending/test-item.md
$AIMEE work add-batch --from-proposals --dir docs/proposals/pending

# 2. Check the source field of the added items
echo "Source field for added items:"
$AIMEE --json work list | grep "source"

# 3. Test resolution with proposal: prefix (full path)
echo "Testing resolution with full path..."
$AIMEE work test-resolve docs/proposals/pending/test-item.md

# 4. Test resolution with short path (fallback)
echo "Testing resolution with short path..."
$AIMEE work test-resolve test-item.md

# 5. Test resolution with moved file
echo "Testing resolution with moved file (accepted)..."
mkdir -p docs/proposals/accepted
mv docs/proposals/pending/test-item.md docs/proposals/accepted/
$AIMEE work test-resolve test-item.md
rm -rf "$HOME"
