# Proposal: Remote Prompt Caching for Delegate and Built-In Chat Providers

## Problem

The original proposal was framed around all aimee sessions, but that is not the
right scope. Primary-agent hook sessions do not call provider APIs through
aimee, so aimee cannot apply provider-side prompt caching there.

The real remaining opportunity is narrower and still valuable:

- delegate agent providers that go through `agent_http.c`
- aimee's built-in chat modes that call remote APIs directly

Those paths still resend large stable prefixes even when context has not
changed.

## Goals

- Reduce repeated prompt cost for delegate and built-in chat API calls
- Add provider-side caching only where aimee actually owns the request payload
- Keep cache state keyed to prompt-prefix content, not whole conversations

## Approach

### 1. Limit scope to owned API paths

Apply remote prompt caching only to:

- delegate providers handled by `agent_http.c`
- built-in chat API providers where aimee constructs the full request

### 2. Cache only the stable prefix

Use a hash over the stable prefix:

- system prompt
- rules
- selected memory context
- project context

The mutable user turn remains outside the cache.

### 3. Provider-specific adapters

- Gemini: cached-content path if available
- Anthropic: prompt caching markers where supported

If the provider rejects or expires the cache, fall back silently to the normal
request.

## Changes

| File | Change |
|------|--------|
| `src/agent_context.c` | Compute stable-prefix hash and cache metadata |
| `src/agent_http.c` | Add provider-specific prompt-caching support |
| `src/cmd_chat.c` | Reuse the same prefix-caching path for built-in chat providers |

## Acceptance Criteria

- [ ] Delegate requests with stable prefixes reuse provider-side caches when supported
- [ ] Built-in chat requests with stable prefixes reuse provider-side caches when supported
- [ ] Cache failures degrade cleanly to uncached requests
- [ ] `agent_log` or adjacent metrics show cache hit/miss visibility

## Owner and Effort

- **Owner:** aimee core
- **Effort:** M
- **Priority:** P2
