# Proposal: Agent Subsystem Streamlining

## Problem
The delegate agent subsystem is spread across 10 files, mixing low-level HTTP/JSON mapping with high-level orchestration and tool execution. This sprawl makes it difficult to maintain the execution loop and scale provider support.

## Goals
- Consolidate 10 files into two primary modules.
- Isolate provider-specific API logic from the core execution engine.

## Approach
Merge the subsystem into two functional layers:
1. **`agent_runtime.c`**: The "Execution Engine." Handles the core loop, tool-use execution, parallel job coordination, and plan evaluation.
2. **`agent_bridge.c`**: The "Provider Layer." Handles configuration, HTTP client wrapping (libcurl), and JSON-RPC mapping for different LLM providers (Anthropic, OpenAI, etc.).

## Acceptance Criteria
- [ ] Agent subsystem reduced from 10 files to 2.
- [ ] `aimee delegate` continues to function for all configured providers.
- [ ] Parallel execution of multi-delegate tasks remains stable.
