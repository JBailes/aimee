# Review: work-queue-audit-trail

## Reviewer context

After processing 7 items, the queue showed 0 done and 0 failed. All my
work was invisible to the queue system. I created PRs for each item but
the queue had no record of this. If someone else looked at the queue,
they would have no idea that work had been done.

## Feedback on approach

### PR correlation

The result field should accept a PR URL. I would suggest `aimee work
complete --result "PR: https://github.com/..."` as the convention, and
have `work list --status done` display it. This is the single most
useful piece of information for tracking parallel work.

### Stats command

Good addition. Would also be useful to see "items completed in the last
24 hours" for quick status checks.

## Priority agreement

P2 is correct as stated. However, the PR correlation aspect is closer to
P1 since without it there is no way to connect queue items to outcomes.
