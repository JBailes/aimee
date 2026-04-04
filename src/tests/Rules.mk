TEST_C_FLAGS = $(C_FLAGS) -I..
TEST_L_FLAGS = $(L_FULL)

# Common object sets for tests
TEST_CORE_OBJS = $(OBJDIR)/db.o $(OBJDIR)/config.o $(OBJDIR)/util.o $(OBJDIR)/text.o \
                 $(OBJDIR)/platform_random.o \
                 $(OBJDIR)/cJSON.o
TEST_DATA_OBJS = $(TEST_CORE_OBJS) $(OBJDIR)/rules.o $(OBJDIR)/feedback.o \
                 $(OBJDIR)/memory.o $(OBJDIR)/memory_promote.o $(OBJDIR)/memory_context.o \
                 $(OBJDIR)/memory_scan.o $(OBJDIR)/memory_graph.o $(OBJDIR)/memory_advanced.o \
                 $(OBJDIR)/index.o $(OBJDIR)/extractors.o $(OBJDIR)/extractors_extra.o \
                 $(OBJDIR)/tasks.o $(OBJDIR)/render.o

TEST_TARGETS = tests/unit-test-util tests/unit-test-db tests/unit-test-rules \
               tests/unit-test-guardrails tests/unit-test-memory tests/unit-test-tasks \
               tests/unit-test-agent tests/unit-test-extractors \
               tests/unit-test-text tests/unit-test-config tests/unit-test-feedback \
               tests/unit-test-render tests/unit-test-index \
               tests/unit-test-memory-advanced tests/unit-test-memory-health \
               tests/unit-test-workspace \
               tests/unit-test-working-memory tests/unit-test-extractors-extra \
               tests/unit-test-compute-pool tests/unit-test-cli-launch \
               tests/unit-test-context-assembly tests/unit-test-workspace-memory \
               tests/unit-test-server-auth \
               tests/unit-test-log tests/unit-test-server-dispatch \
               tests/unit-test-server-compute tests/unit-test-mcp-server \
               tests/unit-test-trace-analysis \
               tests/unit-test-cmd-branch \
               tests/unit-test-cmd-core tests/unit-test-cmd-work \
               tests/unit-test-client-integrations tests/unit-test-mcp-git \
               tests/unit-test-platform-process \
               tests/unit-test-dstr

unit-tests: $(BINARY) $(TEST_TARGETS)
	@for t in $(TEST_TARGETS); do echo "  $$t"; ./$$t || exit 1; done
	@echo "All tests passed."

tests/unit-test-util: $(OBJDIR)/tests/test_util.o $(OBJDIR)/util.o $(OBJDIR)/text.o \
                     $(OBJDIR)/cJSON.o
	$(CC) -o $@ $^ $(TEST_L_FLAGS)

tests/unit-test-db: $(OBJDIR)/tests/test_db.o $(OBJDIR)/db.o $(OBJDIR)/config.o \
                    $(OBJDIR)/util.o $(OBJDIR)/text.o $(OBJDIR)/platform_random.o \
                    $(OBJDIR)/cJSON.o
	$(CC) -o $@ $^ $(TEST_L_FLAGS)

tests/unit-test-rules: $(OBJDIR)/tests/test_rules.o $(OBJDIR)/rules.o $(OBJDIR)/db.o \
                       $(OBJDIR)/config.o $(OBJDIR)/util.o $(OBJDIR)/text.o \
                       $(OBJDIR)/platform_random.o $(OBJDIR)/cJSON.o
	$(CC) -o $@ $^ $(TEST_L_FLAGS)

tests/unit-test-guardrails: $(OBJDIR)/tests/test_guardrails.o $(OBJDIR)/guardrails.o $(OBJDIR)/worktree.o \
                            $(OBJDIR)/db.o $(OBJDIR)/config.o $(OBJDIR)/util.o $(OBJDIR)/text.o \
                            $(OBJDIR)/platform_random.o \
                            $(OBJDIR)/index.o $(OBJDIR)/extractors.o \
                            $(OBJDIR)/extractors_extra.o \
                            $(OBJDIR)/memory.o $(OBJDIR)/memory_promote.o \
                            $(OBJDIR)/memory_context.o $(OBJDIR)/memory_scan.o \
                            $(OBJDIR)/memory_graph.o $(OBJDIR)/memory_advanced.o \
                            $(OBJDIR)/rules.o $(OBJDIR)/feedback.o $(OBJDIR)/tasks.o \
                            $(OBJDIR)/render.o $(OBJDIR)/cJSON.o
	$(CC) -o $@ $^ $(TEST_L_FLAGS)

tests/unit-test-memory: $(OBJDIR)/tests/test_memory.o $(OBJDIR)/memory.o \
                        $(OBJDIR)/memory_promote.o $(OBJDIR)/memory_context.o \
                        $(OBJDIR)/memory_scan.o $(OBJDIR)/memory_graph.o \
                        $(OBJDIR)/memory_advanced.o $(OBJDIR)/db.o $(OBJDIR)/config.o \
                        $(OBJDIR)/util.o $(OBJDIR)/text.o $(OBJDIR)/platform_random.o \
                        $(OBJDIR)/rules.o $(OBJDIR)/feedback.o \
                        $(OBJDIR)/tasks.o $(OBJDIR)/index.o $(OBJDIR)/extractors.o \
                        $(OBJDIR)/extractors_extra.o \
                        $(OBJDIR)/render.o $(OBJDIR)/cJSON.o
	$(CC) -o $@ $^ $(TEST_L_FLAGS)

tests/unit-test-tasks: $(OBJDIR)/tests/test_tasks.o $(OBJDIR)/tasks.o $(OBJDIR)/db.o \
                       $(OBJDIR)/config.o $(OBJDIR)/util.o $(OBJDIR)/text.o \
                       $(OBJDIR)/platform_random.o $(OBJDIR)/memory.o \
                       $(OBJDIR)/memory_promote.o $(OBJDIR)/memory_context.o \
                       $(OBJDIR)/memory_scan.o $(OBJDIR)/memory_graph.o \
                       $(OBJDIR)/memory_advanced.o $(OBJDIR)/rules.o $(OBJDIR)/feedback.o \
                       $(OBJDIR)/index.o $(OBJDIR)/extractors.o \
                       $(OBJDIR)/extractors_extra.o \
                       $(OBJDIR)/render.o $(OBJDIR)/cJSON.o
	$(CC) -o $@ $^ $(TEST_L_FLAGS)

tests/unit-test-agent: $(OBJDIR)/tests/test_agent.o $(OBJDIR)/agent.o $(OBJDIR)/agent_protocol.o \
                      $(OBJDIR)/agent_policy.o \
                      $(OBJDIR)/agent_context.o $(OBJDIR)/agent_config.o \
                      $(OBJDIR)/agent_plan.o $(OBJDIR)/agent_eval.o $(OBJDIR)/agent_coord.o \
                      $(OBJDIR)/agent_jobs.o $(OBJDIR)/agent_tools.o $(OBJDIR)/agent_http.o \
                      $(OBJDIR)/agent_tunnel.o \
                      $(OBJDIR)/db.o $(OBJDIR)/config.o $(OBJDIR)/util.o $(OBJDIR)/text.o \
                      $(OBJDIR)/platform_random.o $(OBJDIR)/cJSON.o \
                      $(OBJDIR)/rules.o $(OBJDIR)/feedback.o $(OBJDIR)/memory.o \
                      $(OBJDIR)/memory_promote.o $(OBJDIR)/memory_context.o \
                      $(OBJDIR)/memory_scan.o $(OBJDIR)/memory_graph.o \
                      $(OBJDIR)/memory_advanced.o $(OBJDIR)/index.o $(OBJDIR)/extractors.o \
                      $(OBJDIR)/extractors_extra.o $(OBJDIR)/guardrails.o $(OBJDIR)/worktree.o \
                      $(OBJDIR)/tasks.o $(OBJDIR)/render.o \
                      $(OBJDIR)/working_memory.o $(OBJDIR)/workspace.o $(OBJDIR)/cmd_describe.o
	$(CC) -o $@ $^ $(TEST_L_FLAGS)

tests/unit-test-extractors: $(OBJDIR)/tests/test_extractors.o $(OBJDIR)/extractors.o \
                           $(OBJDIR)/extractors_extra.o $(OBJDIR)/index.o $(OBJDIR)/db.o \
                           $(OBJDIR)/config.o $(OBJDIR)/util.o $(OBJDIR)/text.o \
                           $(OBJDIR)/platform_random.o \
                           $(OBJDIR)/memory.o $(OBJDIR)/memory_promote.o \
                           $(OBJDIR)/memory_context.o $(OBJDIR)/memory_scan.o \
                           $(OBJDIR)/memory_graph.o $(OBJDIR)/memory_advanced.o \
                           $(OBJDIR)/rules.o $(OBJDIR)/feedback.o $(OBJDIR)/tasks.o \
                           $(OBJDIR)/render.o $(OBJDIR)/cJSON.o
	$(CC) -o $@ $^ $(TEST_L_FLAGS)

# --- New tests ---

tests/unit-test-text: $(OBJDIR)/tests/test_text.o $(OBJDIR)/util.o $(OBJDIR)/text.o \
                     $(OBJDIR)/cJSON.o
	$(CC) -o $@ $^ $(TEST_L_FLAGS)

tests/unit-test-config: $(OBJDIR)/tests/test_config.o $(TEST_CORE_OBJS)
	$(CC) -o $@ $^ $(TEST_L_FLAGS)

tests/unit-test-feedback: $(OBJDIR)/tests/test_feedback.o $(TEST_DATA_OBJS)
	$(CC) -o $@ $^ $(TEST_L_FLAGS)

tests/unit-test-render: $(OBJDIR)/tests/test_render.o $(TEST_DATA_OBJS)
	$(CC) -o $@ $^ $(TEST_L_FLAGS)

tests/unit-test-index: $(OBJDIR)/tests/test_index.o $(TEST_DATA_OBJS)
	$(CC) -o $@ $^ $(TEST_L_FLAGS)

tests/unit-test-memory-advanced: $(OBJDIR)/tests/test_memory_advanced.o $(TEST_DATA_OBJS)
	$(CC) -o $@ $^ $(TEST_L_FLAGS)

tests/unit-test-memory-health: $(OBJDIR)/tests/test_memory_health.o $(TEST_DATA_OBJS)
	$(CC) -o $@ $^ $(TEST_L_FLAGS)

tests/unit-test-workspace: $(OBJDIR)/tests/test_workspace.o $(TEST_DATA_OBJS) \
                          $(OBJDIR)/workspace.o $(OBJDIR)/working_memory.o \
                          $(OBJDIR)/agent_config.o $(OBJDIR)/cmd_describe.o \
                          $(OBJDIR)/guardrails.o $(OBJDIR)/worktree.o $(OBJDIR)/agent.o \
                          $(OBJDIR)/agent_protocol.o $(OBJDIR)/agent_policy.o \
                          $(OBJDIR)/agent_context.o $(OBJDIR)/agent_plan.o $(OBJDIR)/agent_eval.o \
                          $(OBJDIR)/agent_coord.o $(OBJDIR)/agent_jobs.o $(OBJDIR)/agent_tools.o \
                          $(OBJDIR)/agent_http.o $(OBJDIR)/agent_tunnel.o
	$(CC) -o $@ $^ $(TEST_L_FLAGS)

tests/unit-test-working-memory: $(OBJDIR)/tests/test_working_memory.o $(TEST_CORE_OBJS) \
                               $(OBJDIR)/working_memory.o
	$(CC) -o $@ $^ $(TEST_L_FLAGS)

tests/unit-test-extractors-extra: $(OBJDIR)/tests/test_extractors_extra.o $(TEST_DATA_OBJS)
	$(CC) -o $@ $^ $(TEST_L_FLAGS)

tests/unit-test-compute-pool: $(OBJDIR)/tests/test_compute_pool.o $(OBJDIR)/compute_pool.o
	$(CC) -o $@ $^ $(L_MINIMAL)

tests/unit-test-cli-launch: $(OBJDIR)/tests/test_cli_launch.o $(OBJDIR)/cli_launch.o \
                            $(OBJDIR)/cJSON.o
	$(CC) -o $@ $^ $(TEST_L_FLAGS)

tests/unit-test-context-assembly: $(OBJDIR)/tests/test_context_assembly.o $(TEST_DATA_OBJS) \
                                 $(OBJDIR)/agent_context.o $(OBJDIR)/agent.o \
                                 $(OBJDIR)/agent_protocol.o $(OBJDIR)/agent_policy.o \
                                 $(OBJDIR)/agent_config.o \
                                 $(OBJDIR)/agent_plan.o $(OBJDIR)/agent_eval.o \
                                 $(OBJDIR)/agent_coord.o $(OBJDIR)/agent_jobs.o \
                                 $(OBJDIR)/agent_tools.o $(OBJDIR)/agent_http.o \
                                 $(OBJDIR)/agent_tunnel.o $(OBJDIR)/working_memory.o \
                                 $(OBJDIR)/guardrails.o $(OBJDIR)/worktree.o $(OBJDIR)/workspace.o \
                                 $(OBJDIR)/cmd_describe.o
	$(CC) -o $@ $^ $(TEST_L_FLAGS)

tests/unit-test-workspace-memory: $(OBJDIR)/tests/test_workspace_memory.o $(TEST_DATA_OBJS)
	$(CC) -o $@ $^ $(TEST_L_FLAGS)

tests/unit-test-server-auth: $(OBJDIR)/tests/test_server_auth.o $(OBJDIR)/server_auth.o \
                            $(OBJDIR)/secret_store.o $(OBJDIR)/log.o $(TEST_CORE_OBJS)
	$(CC) -o $@ $^ $(TEST_L_FLAGS)

tests/unit-test-log: $(OBJDIR)/tests/test_log.o $(OBJDIR)/log.o $(OBJDIR)/config.o \
                    $(OBJDIR)/util.o $(OBJDIR)/text.o $(OBJDIR)/platform_random.o \
                    $(OBJDIR)/db.o $(OBJDIR)/cJSON.o
	$(CC) -o $@ $^ $(TEST_L_FLAGS)

tests/unit-test-server-dispatch: $(OBJDIR)/tests/test_server_dispatch.o $(OBJDIR)/server.o \
                                $(OBJDIR)/cJSON.o
	$(CC) -o $@ $^ $(TEST_L_FLAGS)

tests/unit-test-server-compute: $(OBJDIR)/tests/test_server_compute.o $(OBJDIR)/db.o \
                               $(OBJDIR)/config.o $(OBJDIR)/util.o $(OBJDIR)/text.o \
                               $(OBJDIR)/platform_random.o $(OBJDIR)/cJSON.o
	$(CC) -o $@ $^ $(TEST_L_FLAGS)

tests/unit-test-mcp-server: $(OBJDIR)/tests/test_mcp_server.o $(TEST_DATA_OBJS) \
                            $(OBJDIR)/working_memory.o $(OBJDIR)/log.o \
                            $(OBJDIR)/mcp_git.o $(OBJDIR)/git_verify.o \
                            $(OBJDIR)/guardrails.o $(OBJDIR)/worktree.o
	$(CC) -o $@ $^ $(TEST_L_FLAGS)

tests/unit-test-trace-analysis: $(OBJDIR)/tests/test_trace_analysis.o $(TEST_DATA_OBJS) \
                                $(OBJDIR)/trace_analysis.o
	$(CC) -o $@ $^ $(TEST_L_FLAGS)

tests/unit-test-cmd-branch: $(OBJDIR)/tests/test_cmd_branch.o $(OBJDIR)/cmd_branch.o \
                           $(OBJDIR)/cmd_util.o $(TEST_DATA_OBJS) \
                           $(OBJDIR)/mcp_git.o $(OBJDIR)/git_verify.o \
                           $(OBJDIR)/working_memory.o $(OBJDIR)/guardrails.o $(OBJDIR)/worktree.o
	$(CC) -o $@ $^ $(TEST_L_FLAGS)

tests/unit-test-cmd-core: $(OBJDIR)/tests/test_cmd_core.o $(TEST_DATA_OBJS) \
                         $(OBJDIR)/cmd_util.o $(OBJDIR)/cmd_work.o \
                         $(OBJDIR)/mcp_git.o $(OBJDIR)/git_verify.o \
                         $(OBJDIR)/guardrails.o $(OBJDIR)/worktree.o
	$(CC) -o $@ $^ $(TEST_L_FLAGS)

tests/unit-test-cmd-work: $(OBJDIR)/tests/test_cmd_work.o $(TEST_DATA_OBJS) \
                          $(OBJDIR)/cmd_work.o $(OBJDIR)/cmd_util.o
	$(CC) -o $@ $^ $(TEST_L_FLAGS)

tests/unit-test-client-integrations: $(OBJDIR)/tests/test_client_integrations.o $(TEST_CORE_OBJS)
	$(CC) -o $@ $^ $(TEST_L_FLAGS)

tests/unit-test-mcp-git: $(OBJDIR)/tests/test_mcp_git.o $(OBJDIR)/mcp_git.o \
                        $(OBJDIR)/git_verify.o $(OBJDIR)/cJSON.o \
                        $(OBJDIR)/util.o $(OBJDIR)/text.o \
                        $(OBJDIR)/db.o $(OBJDIR)/config.o $(OBJDIR)/platform_random.o \
                        $(OBJDIR)/guardrails.o $(OBJDIR)/worktree.o $(OBJDIR)/index.o \
                        $(OBJDIR)/extractors.o $(OBJDIR)/extractors_extra.o \
                        $(OBJDIR)/memory.o $(OBJDIR)/memory_promote.o \
                        $(OBJDIR)/memory_context.o $(OBJDIR)/memory_scan.o \
                        $(OBJDIR)/memory_graph.o $(OBJDIR)/memory_advanced.o \
                        $(OBJDIR)/rules.o $(OBJDIR)/feedback.o \
                        $(OBJDIR)/tasks.o $(OBJDIR)/render.o
	$(CC) -o $@ $^ $(TEST_L_FLAGS)

tests/unit-test-platform-process: $(OBJDIR)/tests/test_platform_process.o \
                                  $(OBJDIR)/platform_process.o
	$(CC) -o $@ $^ $(L_MINIMAL)

tests/unit-test-dstr: $(OBJDIR)/tests/test_dstr.o $(OBJDIR)/dstr.o
	$(CC) -o $@ $^ $(L_MINIMAL)

$(OBJDIR)/tests/%.o: tests/%.c
	@mkdir -p $(OBJDIR)/tests
	$(CC) -c $(TEST_C_FLAGS) -o $@ $<
