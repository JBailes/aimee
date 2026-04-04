CC      = gcc
OBJDIR  = build/obj
BINARY  = ../aimee
SERVER  = ../aimee-server

C_FLAGS = -O2 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wno-format-truncation -Wno-unused-result -MMD -MP -Iheaders -Ivendor/headers $(EXTRA_C_FLAGS)

UNAME_S := $(shell uname -s)

# Platform-specific HTTP library
ifeq ($(OS),Windows_NT)
    HTTP_LIB = -lwinhttp
else
    HTTP_LIB =
endif

# PAM support (optional: set WITH_PAM=1 to enable, auto-detected if libpam-dev present)
WITH_PAM ?= $(shell echo '\#include <security/pam_appl.h>' | $(CC) -xc -fsyntax-only - 2>/dev/null && echo 1 || echo 0)
ifeq ($(WITH_PAM),1)
    PAM_FLAGS = -DWITH_PAM
    PAM_LIBS  = -lpam
else
    PAM_FLAGS =
    PAM_LIBS  =
endif

# libsecret support (optional: set WITH_LIBSECRET=1 to enable, auto-detected if libsecret-1-dev present)
WITH_LIBSECRET ?= $(shell pkg-config --exists libsecret-1 2>/dev/null && echo 1 || echo 0)
ifeq ($(WITH_LIBSECRET),1)
    LIBSECRET_FLAGS = -DWITH_LIBSECRET $(shell pkg-config --cflags libsecret-1)
    LIBSECRET_LIBS  = $(shell pkg-config --libs libsecret-1)
else
    LIBSECRET_FLAGS =
    LIBSECRET_LIBS  =
endif

C_FLAGS += $(PAM_FLAGS) $(LIBSECRET_FLAGS)

ifeq ($(UNAME_S),Darwin)
    PLATFORM_FRAMEWORKS = -framework Security -framework CoreFoundation
else
    PLATFORM_FRAMEWORKS =
endif

# Linker flag sets (layered -- each level adds to the previous)
L_MINIMAL = -O2 -g -lpthread $(EXTRA_L_FLAGS)
L_CORE    = -O2 -g -lsqlite3 -lm -lpthread $(EXTRA_L_FLAGS)
L_AGENT   = $(L_CORE) -lssl -lcrypto $(HTTP_LIB)
L_FULL    = $(L_AGENT) $(PAM_LIBS) $(LIBSECRET_LIBS) $(PLATFORM_FRAMEWORKS)

# --- Layer 0: Foundation (db, config, util, text, render, log, platform, cJSON) ---
CORE_SRCS = db.c config.c util.c text.c render.c log.c dstr.c \
            platform_event.c platform_ipc.c platform_random.c platform_process.c \
            client_integrations.c mcp_git.c mcp_tools.c git_verify.c
CORE_OBJS = $(CORE_SRCS:%.c=$(OBJDIR)/%.o) $(OBJDIR)/cJSON.o

# --- Layer 1: Data & policy (memory, index, rules, guardrails, workspace) ---
DATA_SRCS = memory.c memory_promote.c memory_context.c memory_scan.c \
            memory_graph.c memory_advanced.c trace_analysis.c \
            index.c extractors.c extractors_extra.c \
            rules.c tasks.c feedback.c guardrails.c worktree.c \
            workspace.c working_memory.c agent_config.c cmd_describe.c \
            secret_store.c
DATA_OBJS = $(DATA_SRCS:%.c=$(OBJDIR)/%.o)

# --- Layer 2: Agent execution (agent loop, tools, HTTP, policy) ---
AGENT_SRCS = agent.c agent_policy.c agent_context.c \
             agent_plan.c agent_eval.c agent_coord.c agent_jobs.c \
             agent_tools.c agent_http.c agent_tunnel.c
AGENT_OBJS = $(AGENT_SRCS:%.c=$(OBJDIR)/%.o)

# --- Layer 3: Commands + UI (cmd_*, webchat, dashboard) ---
CMD_SRCS = cmd_core.c cmd_data.c cmd_infra.c cmd_memory.c cmd_index.c cmd_rules.c \
           cmd_hooks.c cmd_agent.c cmd_agent_setup.c cmd_agent_trace.c cmd_agent_delegate.c cmd_agent_plan.c \
           cmd_chat.c cmd_wm.c cmd_work.c cmd_branch.c cmd_util.c cmd_table.c \
           dashboard.c webchat.c
CMD_OBJS = $(CMD_SRCS:%.c=$(OBJDIR)/%.o)

# --- Pure client (aimee) ---
CLI_SRCS = cli_main.c cli_client.c cli_launch.c cli_mcp_serve.c client_integrations.c
CLI_OBJS = $(CLI_SRCS:%.c=$(OBJDIR)/%.o)

# --- Server ---
SERVER_SRCS = server_main.c server.c server_auth.c server_session.c server_state.c \
              server_compute.c server_forward.c server_mcp.c compute_pool.c cli_client.c
SERVER_OBJS = $(SERVER_SRCS:%.c=$(OBJDIR)/%.o)

# All source files for linting (excluding vendored cJSON)
ALL_SRCS = $(sort $(CORE_SRCS) $(DATA_SRCS) $(AGENT_SRCS) $(CMD_SRCS) \
           $(CLI_SRCS) $(SERVER_SRCS))
LINT_SRCS = $(ALL_SRCS) $(wildcard tests/test_*.c) $(wildcard tests/fuzz_*.c) $(wildcard tests/bench_*.c)

# All object files for dependency tracking
ALL_OBJS = $(CORE_OBJS) $(DATA_OBJS) $(AGENT_OBJS) $(CMD_OBJS) \
           $(CLI_OBJS) $(SERVER_OBJS)
DEPS = $(ALL_OBJS:.o=.d)

.PHONY: all clean install lint format unit-tests integration-tests server fuzz coverage static-analysis cppcheck clang-tidy bench bench-check bench-baseline build-integrity

all: $(BINARY)

# Test targets (included after 'all' so unit-tests is not the default target)
include tests/Rules.mk

server: $(SERVER)

integration-tests: $(BINARY) $(SERVER)
	./tests/test_integration.sh

# --- Benchmark suite ---
BENCH_BASELINE = ../benchmarks/baseline.json

BENCH_OBJS = $(OBJDIR)/tests/bench_perf.o $(OBJDIR)/guardrails.o $(OBJDIR)/worktree.o \
             $(OBJDIR)/workspace.o $(OBJDIR)/working_memory.o \
             $(OBJDIR)/agent_config.o $(OBJDIR)/cmd_describe.o \
             $(AGENT_OBJS)

tests/bench-perf: $(BENCH_OBJS) $(TEST_DATA_OBJS)
	$(CC) -o $@ $^ $(TEST_L_FLAGS)

bench: tests/bench-perf
	./tests/bench-perf

bench-check: tests/bench-perf
	./tests/bench-perf --check $(BENCH_BASELINE)

bench-baseline: tests/bench-perf
	./tests/bench-perf --save $(BENCH_BASELINE)
	@echo "Baseline updated. Commit benchmarks/baseline.json to persist."

# --- Libraries (layered) ---
libaimee-core.a: $(CORE_OBJS)
	ar rcs $@ $^

libaimee-data.a: $(DATA_OBJS)
	ar rcs $@ $^

libaimee-agent.a: $(AGENT_OBJS)
	ar rcs $@ $^

libaimee-cmd.a: $(CMD_OBJS)
	ar rcs $@ $^

# --- Binaries ---

# Pure client (includes MCP proxy via cli_mcp_serve.c)
$(BINARY): $(CLI_OBJS) libaimee-core.a
	$(CC) -o $@ $^ $(L_CORE)

# Server (all layers + server code)
$(SERVER): $(SERVER_OBJS) libaimee-cmd.a libaimee-agent.a libaimee-data.a libaimee-core.a
	$(CC) -o $@ $^ $(L_FULL)

# --- Compilation ---

$(OBJDIR)/%.o: %.c
	@mkdir -p $(OBJDIR)
	$(CC) -c $(C_FLAGS) -o $@ $<

$(OBJDIR)/cJSON.o: vendor/cJSON.c
	@mkdir -p $(OBJDIR)
	$(CC) -c -O2 -g -w -Ivendor/headers -o $@ $<

PREFIX ?= /usr/local

install: $(BINARY) $(SERVER)
	install -m 755 $(BINARY) $(PREFIX)/bin/aimee
	install -m 755 $(SERVER) $(PREFIX)/bin/aimee-server

clean:
	rm -rf build $(BINARY) $(SERVER) \
	       libaimee-core.a libaimee-data.a libaimee-agent.a libaimee-cmd.a \
	       tests/unit-test-* tests/bench-perf tests/fuzz_extractors tests/fuzz_json_parse \
	       tests/fuzz_config_load tests/fuzz_memory_search \
	       coverage.info coverage-report

lint:
	@clang-format --dry-run -Werror $(LINT_SRCS) && echo "lint: ok"

format:
	clang-format -i $(LINT_SRCS)

# --- Fuzz testing (standalone mode, no libFuzzer required) ---

FUZZ_COMMON_OBJS = $(OBJDIR)/db.o $(OBJDIR)/config.o $(OBJDIR)/util.o $(OBJDIR)/text.o \
                   $(OBJDIR)/platform_random.o \
                   $(OBJDIR)/memory.o $(OBJDIR)/memory_promote.o \
                   $(OBJDIR)/memory_context.o $(OBJDIR)/memory_scan.o \
                   $(OBJDIR)/memory_graph.o $(OBJDIR)/memory_advanced.o \
                   $(OBJDIR)/rules.o $(OBJDIR)/feedback.o $(OBJDIR)/tasks.o \
                   $(OBJDIR)/render.o $(OBJDIR)/cJSON.o

FUZZ_EXTRACTOR_OBJS = $(OBJDIR)/tests/fuzz_extractors.o $(OBJDIR)/extractors.o \
                      $(OBJDIR)/extractors_extra.o $(OBJDIR)/index.o $(FUZZ_COMMON_OBJS)

FUZZ_JSON_OBJS = $(OBJDIR)/tests/fuzz_json_parse.o $(OBJDIR)/cJSON.o

FUZZ_CONFIG_OBJS = $(OBJDIR)/tests/fuzz_config_load.o $(FUZZ_COMMON_OBJS)

FUZZ_MEMSEARCH_OBJS = $(OBJDIR)/tests/fuzz_memory_search.o $(FUZZ_COMMON_OBJS) \
                      $(OBJDIR)/index.o $(OBJDIR)/extractors.o $(OBJDIR)/extractors_extra.o

FUZZ_TARGETS = tests/fuzz_extractors tests/fuzz_json_parse tests/fuzz_config_load \
               tests/fuzz_memory_search

$(OBJDIR)/tests/fuzz_extractors.o: tests/fuzz_extractors.c
	@mkdir -p $(OBJDIR)/tests
	$(CC) -c $(C_FLAGS) -DFUZZ_STANDALONE -o $@ $<

$(OBJDIR)/tests/fuzz_json_parse.o: tests/fuzz_json_parse.c
	@mkdir -p $(OBJDIR)/tests
	$(CC) -c $(C_FLAGS) -DFUZZ_STANDALONE -o $@ $<

$(OBJDIR)/tests/fuzz_config_load.o: tests/fuzz_config_load.c
	@mkdir -p $(OBJDIR)/tests
	$(CC) -c $(C_FLAGS) -DFUZZ_STANDALONE -o $@ $<

$(OBJDIR)/tests/fuzz_memory_search.o: tests/fuzz_memory_search.c
	@mkdir -p $(OBJDIR)/tests
	$(CC) -c $(C_FLAGS) -DFUZZ_STANDALONE -o $@ $<

tests/fuzz_extractors: $(FUZZ_EXTRACTOR_OBJS)
	$(CC) -o $@ $^ $(L_FULL)

tests/fuzz_json_parse: $(FUZZ_JSON_OBJS)
	$(CC) -o $@ $^ $(L_CORE)

tests/fuzz_config_load: $(FUZZ_CONFIG_OBJS)
	$(CC) -o $@ $^ $(L_CORE)

tests/fuzz_memory_search: $(FUZZ_MEMSEARCH_OBJS)
	$(CC) -o $@ $^ $(L_CORE)

fuzz: $(FUZZ_TARGETS)
	./tests/fuzz_extractors tests/fuzz_corpus/seed.*
	./tests/fuzz_json_parse tests/fuzz_corpus/seed.json tests/fuzz_corpus/seed_config.json
	./tests/fuzz_config_load tests/fuzz_corpus/seed_config.json
	./tests/fuzz_memory_search tests/fuzz_corpus/seed_query.txt

# --- Coverage reporting ---

coverage:
	$(MAKE) clean
	$(MAKE) unit-tests EXTRA_C_FLAGS="--coverage" EXTRA_L_FLAGS="--coverage"
	lcov --capture --directory build/obj --output-file coverage.info --quiet \
		--ignore-errors inconsistent
	lcov --remove coverage.info '*/vendor/*' '*/tests/*' --output-file coverage.info --quiet \
		--ignore-errors inconsistent,unused
	genhtml coverage.info --output-directory coverage-report --quiet \
		--ignore-errors inconsistent
	@lcov --summary coverage.info 2>&1 | grep -E 'lines|functions'
	@echo "Coverage report: coverage-report/index.html"

coverage-check: coverage
	@echo "Checking coverage floors for high-risk modules..."
	@fail=0; \
	for spec in server.c:80 server_auth.c:90 config.c:80 memory.c:70; do \
		file=$${spec%%:*}; floor=$${spec##*:}; \
		pct=$$(lcov --summary coverage.info --quiet 2>&1 \
			| grep -oP '[\d.]+(?=%)' | head -1); \
		file_pct=$$(lcov --extract coverage.info "*/$${file}" --quiet \
			--ignore-errors inconsistent 2>&1 \
			| lcov --summary /dev/stdin --quiet 2>&1 \
			| grep -oP 'lines.*?(\d+\.\d+)' | grep -oP '[\d.]+' | tail -1); \
		if [ -n "$$file_pct" ]; then \
			ok=$$(echo "$$file_pct >= $$floor" | bc -l 2>/dev/null || echo 1); \
			if [ "$$ok" = "0" ]; then \
				echo "FAIL: $$file coverage $${file_pct}% < $${floor}% floor"; \
				fail=1; \
			else \
				echo "PASS: $$file coverage $${file_pct}% >= $${floor}% floor"; \
			fi; \
		else \
			echo "SKIP: $$file (no coverage data)"; \
		fi; \
	done; \
	if [ "$$fail" = "1" ]; then echo "Coverage floor check failed."; exit 1; fi; \
	echo "All coverage floors met."

# --- Static analysis ---

STATIC_ANALYSIS_STRICT ?= 0

cppcheck:
	@if command -v cppcheck >/dev/null 2>&1; then \
		cppcheck -j $$(nproc 2>/dev/null || echo 2) \
			--enable=warning,performance,portability \
			--suppress=missingIncludeSystem \
			--suppress=nullPointerOutOfMemory \
			--suppress=normalCheckLevelMaxBranches \
			--error-exitcode=1 -Iheaders -Ivendor/headers \
			$(ALL_SRCS) && echo "cppcheck: ok"; \
	elif [ "$(STATIC_ANALYSIS_STRICT)" = "1" ]; then \
		echo "ERROR: cppcheck not found. Install with: apt install cppcheck"; exit 1; \
	else \
		echo "SKIP: cppcheck not found. Install with: apt install cppcheck"; \
	fi

clang-tidy:
	@if command -v clang-tidy >/dev/null 2>&1; then \
		cd . && find . -maxdepth 1 -name '*.c' -print0 \
			| xargs -0 -P "$$(nproc 2>/dev/null || echo 2)" -n 4 \
			  clang-tidy -- -Iheaders -Ivendor/headers -DWITH_PAM \
			2>&1 | tee /tmp/tidy.log; \
		echo "clang-tidy warnings: $$(grep -c 'warning:' /tmp/tidy.log 2>/dev/null || echo 0)"; \
	elif [ "$(STATIC_ANALYSIS_STRICT)" = "1" ]; then \
		echo "ERROR: clang-tidy not found. Install with: apt install clang-tidy"; exit 1; \
	else \
		echo "SKIP: clang-tidy not found. Install with: apt install clang-tidy"; \
	fi

static-analysis: cppcheck clang-tidy

build-integrity:
	./tests/test_build_integrity.sh

-include $(DEPS)
