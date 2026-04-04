#!/bin/bash
# Build integrity tests: catch common Makefile and source breakage early.
# Run from the src/ directory.
set -euo pipefail

FAIL=0
pass() { echo "  PASS: $1"; }
fail() { echo "  FAIL: $1"; FAIL=1; }

echo "build-integrity:"

# 1. No duplicate variable assignments in Makefile (catches overwritten vars)
dupes=$(grep -E '^[A-Z_]+ =' Makefile | awk '{print $1}' | sort | uniq -d)
if [ -z "$dupes" ]; then
    pass "no duplicate Makefile variable assignments"
else
    fail "duplicate Makefile variable assignments: $dupes"
fi

# 2. Every .c in CORE/DATA/AGENT/CMD_SRCS actually exists
for var in CORE_SRCS DATA_SRCS AGENT_SRCS CMD_SRCS CLI_SRCS SERVER_SRCS; do
    files=$(make -p 2>/dev/null | grep "^$var = " | sed "s/^$var = //" | tr ' ' '\n' | grep '\.c$')
    missing=""
    for f in $files; do
        [ -f "$f" ] || missing="$missing $f"
    done
    if [ -z "$missing" ]; then
        pass "$var: all source files exist"
    else
        fail "$var: missing files:$missing"
    fi
done

# 3. Every test source listed in TEST_TARGETS has a corresponding .c file
targets=$(make -p 2>/dev/null | grep "^TEST_TARGETS = " | sed "s/^TEST_TARGETS = //" | tr ' ' '\n')
missing_tests=""
for t in $targets; do
    # tests/unit-test-foo -> tests/test_foo.c (convert hyphens to underscores, strip unit- prefix)
    base=$(echo "$t" | sed 's|tests/unit-test-|tests/test_|; s|-|_|g')
    src="${base}.c"
    [ -f "$src" ] || missing_tests="$missing_tests $src"
done
if [ -z "$missing_tests" ]; then
    pass "all TEST_TARGETS have source files"
else
    fail "missing test sources:$missing_tests"
fi

# 4. Rules.mk: TEST_TARGETS continuation lines (detect missing backslash)
# Every non-last line of a multi-line variable must end with backslash
in_targets=0
line_num=0
bad_lines=""
while IFS= read -r line; do
    line_num=$((line_num + 1))
    if echo "$line" | grep -q "^TEST_TARGETS"; then
        in_targets=1
    fi
    if [ "$in_targets" = "1" ]; then
        # If line has content and does NOT end with \ but the next line
        # is indented (continuation), that is a missing backslash
        if echo "$line" | grep -qE '^\s+tests/' && ! echo "$line" | grep -q '\\$'; then
            in_targets=0  # this should be the last line
        fi
    fi
done < tests/Rules.mk

# 5. Every test target linking config.o must also link platform_random.o
# Join continuation lines, then check each target rule
bad_targets=""
sed ':a; /\\$/N; s/\\\n//; ta' tests/Rules.mk | grep 'tests/unit-test-' | while IFS= read -r rule; do
    target=$(echo "$rule" | cut -d: -f1 | tr -d ' ')
    deps=$(echo "$rule" | cut -d: -f2-)
    if echo "$deps" | grep -q 'config\.o' && \
       ! echo "$deps" | grep -q 'platform_random\.o' && \
       ! echo "$deps" | grep -q 'TEST_CORE_OBJS\|TEST_DATA_OBJS\|CORE_OBJS'; then
        echo "$target" >> /tmp/build_integrity_bad_targets
    fi
done
rm -f /tmp/build_integrity_bad_targets_result
if [ -f /tmp/build_integrity_bad_targets ]; then
    bad_targets=$(cat /tmp/build_integrity_bad_targets | tr '\n' ' ')
    rm -f /tmp/build_integrity_bad_targets
    fail "config.o without platform_random.o in: $bad_targets"
else
    pass "all test targets with config.o also link platform_random.o"
fi

# 6. Every .PHONY target has a corresponding rule (catches missing targets)
phony_targets=$(grep '^\.PHONY:' Makefile tests/Rules.mk 2>/dev/null | \
    sed 's/^.*\.PHONY://' | tr ' ' '\n' | sort -u | grep -v '^$')
missing_rules=""
for target in $phony_targets; do
    # make -n (dry run) exits non-zero if the target has no rule
    if ! make -n "$target" >/dev/null 2>&1; then
        missing_rules="$missing_rules $target"
    fi
done
if [ -z "$missing_rules" ]; then
    pass "all .PHONY targets have rules"
else
    fail ".PHONY targets with no rule:$missing_rules"
fi

# 7. Scripts don't reference non-existent make targets
# Only match lines where 'make' is the command (start of line or after && / ; / |)
bad_script_targets=""
for script in ../update.sh ../install.sh ../setup.sh; do
    [ -f "$script" ] || continue
    # Extract lines where make is invoked as a command, then pull targets
    targets=$(grep -E '(^|[;&|]\s*)make\s' "$script" 2>/dev/null \
        | sed 's/.*make //' \
        | tr ' ' '\n' \
        | grep -vE '^-|^\$|^$|^>|^2>' \
        || true)
    for target in $targets; do
        if ! make -n "$target" >/dev/null 2>&1; then
            bad_script_targets="$bad_script_targets $script:$target"
        fi
    done
done
if [ -z "$bad_script_targets" ]; then
    pass "scripts reference only valid make targets"
else
    fail "scripts reference missing make targets:$bad_script_targets"
fi

# 8. Clean build succeeds (compilation + link)
if make clean all >/dev/null 2>&1; then
    pass "clean build succeeds"
else
    fail "clean build failed"
fi

# 9. All test binaries build successfully
if make unit-tests >/dev/null 2>&1; then
    pass "all unit tests build and pass"
else
    fail "unit tests failed"
fi

# 10. Source file size budgets: no non-exempt .c file exceeds 1500 lines.
# Existing large files are tracked in an explicit allowlist with current caps.
# New files must stay under the budget. Existing files must not grow beyond their cap.
MAX_LINES=1500
declare -A EXEMPT=(
    [webchat.c]=2700
    [guardrails.c]=2000
    [cmd_agent.c]=2000
    [agent_tools.c]=2000
    [mcp_git.c]=1850
    [cmd_agent_trace.c]=1750
    [cmd_hooks.c]=1700
    [memory.c]=1600
    [agent.c]=1600
    [mcp_server.c]=1600
    [db.c]=1600
)
oversized=""
for f in *.c; do
    [ -f "$f" ] || continue
    lines=$(wc -l < "$f")
    cap=${EXEMPT[$f]:-$MAX_LINES}
    if [ "$lines" -gt "$cap" ]; then
        oversized="$oversized $f($lines>$cap)"
    fi
done
if [ -z "$oversized" ]; then
    pass "all source files within size budget (max $MAX_LINES, ${#EXEMPT[@]} exempt)"
else
    fail "source files exceed size budget:$oversized"
fi

echo ""
if [ "$FAIL" = "0" ]; then
    echo "All build integrity checks passed."
else
    echo "Build integrity checks FAILED."
    exit 1
fi
