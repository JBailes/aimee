#!/bin/bash
# aimee performance benchmarks
# Measures critical-path latency: startup, hook calls, memory search, context assembly
#
# Usage: ./benchmarks/run.sh [iterations]
# Output: tab-separated results with p50/p95/p99 percentiles

set -euo pipefail

ITERATIONS=${1:-100}
AIMEE=${AIMEE:-aimee}
RESULTS_DIR="benchmarks/results"
mkdir -p "$RESULTS_DIR"

echo "=== aimee benchmark suite ==="
echo "Binary: $(which "$AIMEE" 2>/dev/null || echo "$AIMEE")"
echo "Iterations: $ITERATIONS"
echo ""

# Helper: run N iterations, collect timings in ms
bench() {
   local name="$1"
   shift
   local timings=()

   for ((i = 0; i < ITERATIONS; i++)); do
      local start
      start=$(date +%s%N)
      "$@" >/dev/null 2>&1 || true
      local end
      end=$(date +%s%N)
      local ms=$(( (end - start) / 1000000 ))
      timings+=("$ms")
   done

   # Sort and compute percentiles
   IFS=$'\n' sorted=($(sort -n <<<"${timings[*]}")); unset IFS
   local count=${#sorted[@]}
   local p50=${sorted[$((count * 50 / 100))]}
   local p95=${sorted[$((count * 95 / 100))]}
   local p99=${sorted[$((count * 99 / 100))]}
   local min=${sorted[0]}
   local max=${sorted[$((count - 1))]}

   printf "%-30s  p50=%3dms  p95=%3dms  p99=%3dms  min=%3dms  max=%3dms\n" \
      "$name" "$p50" "$p95" "$p99" "$min" "$max"

   # Save raw timings
   printf "%s\n" "${timings[@]}" > "$RESULTS_DIR/${name// /_}.txt"
}

# 1. Binary startup (--version)
bench "startup (version)" "$AIMEE" version

# 2. Hook: pre-tool check (Edit, simple path)
bench "hooks pre (edit)" bash -c "echo '{\"tool_name\":\"Edit\",\"tool_input\":\"{\\\"file_path\\\":\\\"/tmp/test.txt\\\"}\"}' | $AIMEE hooks pre"

# 3. Hook: pre-tool check (Bash, simple command)
bench "hooks pre (bash)" bash -c "echo '{\"tool_name\":\"Bash\",\"tool_input\":\"{\\\"command\\\":\\\"ls\\\"}\"}' | $AIMEE hooks pre"

# 4. Memory search (FTS5)
bench "memory search (fts5)" "$AIMEE" --json memory search proxmox

# 5. Memory list (L2 facts)
bench "memory list (L2 facts)" "$AIMEE" --json memory list --tier L2 --kind fact

# 6. Agent network
bench "agent network" "$AIMEE" agent network

# 7. Session-start (full context assembly)
bench "session-start" "$AIMEE" session-start

# 8. Memory maintain (promotion/demotion cycle)
bench "memory maintain" "$AIMEE" --json memory maintain

echo ""
echo "Results saved to $RESULTS_DIR/"
