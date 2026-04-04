# Review: work-queue-stale-claim-gc (Session 2)

## Reviewer context

I observed the same stale claim problem. When I started processing the
queue, several items were already claimed by sessions that had long since
finished. `aimee work claim` skipped over these and gave me the next
pending item, but the stale claims remained visible and confusing in
`work list` output.

## Feedback on approach

### Auto-release timeout

The 2-hour timeout is reasonable. I would also suggest displaying the
`claimed_at` timestamp in `work list` output so users can see at a glance
which claims are stale. Currently the output shows the session ID but not
when the claim happened, making it hard to judge staleness.

### `aimee work gc`

Good addition. I would also suggest running GC automatically during
`aimee work list` (not just during `claim`). When a user runs `work list`
to assess the queue state, they should see the accurate state, not stale
data.

## Priority agreement

P1. Confirmed independently. The queue degrades over time without this.
