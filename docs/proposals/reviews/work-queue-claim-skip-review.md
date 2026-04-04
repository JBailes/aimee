# Review: work-queue-claim-skip

## Reviewer context

Hit this hard during a full queue processing session. I had to claim and
abandon 4 consecutive file-splitting refactors (split-agent-tools,
split-webchat, split-cmd-agent, dry-refactoring) before finding a tractable
item (static-analysis-robustness). Each claim left the item stuck in
"claimed" status (due to the session-fix bug), polluting the queue.

The original proposal covered `--skip N` which helps positionally, but the
root problem is that skip-by-position requires knowing which items to skip
without reading their proposals. The proposal has been updated to include
metadata-based filtering.

## Feedback on updated approach

### `--skip N` flag

Still good for simple cases. Kept as-is.

### Metadata columns (effort, tags)

The key addition. During my session, the queue mixed S/M/L effort items and
feature/refactoring types with no way to distinguish them. Adding `effort`
and `tags` columns with `--effort S` and `--tag feature` / `--exclude-tag
refactor` flags on `claim` would have let me skip all the large refactoring
items in one shot instead of claiming them one by one.

### Auto-extract in add-batch

Parsing effort from `**Effort:** S` in proposal markdown is straightforward
regex. Priority extraction from `## Priority` (P0=30, P1=20, etc.) is also
well-defined. Both are worth doing to avoid manual metadata entry.

### --skip-source pattern

The earlier review suggested `--skip-source "proposal:split-*"`. This is
complementary to tags but more ad-hoc. Tags are the better long-term
solution since they work across all item sources, not just proposals.

## Priority agreement

P2 is correct. The metadata filtering makes it significantly more useful
than skip-by-position alone.
