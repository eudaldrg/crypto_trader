---
aliases: [pr, pull request, gh pr, force push]
---

# Pull requests

How to open, update and review a pull request in this repository, and the one
`gh` failure that looks like success.

Opening or merging a PR, and pushing, need the owner's explicit go-ahead each
time; a yes to one does not carry over to the next. Branches are `feature/...`,
PRs are squash-merged.

## Updating a PR's title or body

`gh pr edit` can fail silently here. With `gh` 2.46 it printed

```
GraphQL: Projects (classic) is being deprecated in favor of the new Projects experience ...
```

and left the old title and body in place, which reads like a warning on a
successful edit. Update through the REST API instead, and read the result back:

```bash
gh api -X PATCH repos/<owner>/<repo>/pulls/<n> -f title="..." -F body=@body.md
gh api repos/<owner>/<repo>/pulls/<n> --jq .body | head
```

Keep the body in a file (`-F body=@file`) rather than inline, so quoting and
newlines survive.

## Reviewing what a branch adds

Compare against the merge-base with three dots, so commits that landed on
`main` since the branch was cut do not show up as removals:

```bash
git diff --stat origin/main...HEAD     # the whole branch, per file
git log --stat origin/main..HEAD       # commit by commit
```

`gh pr diff <n>` shows what is on GitHub, not local commits that are not
pushed yet.

## After rewriting local history

If commits were reshuffled or squashed locally, the branch has diverged from its
remote copy and needs a force push. Use `--force-with-lease`, which refuses if
the remote moved since you last fetched, and only when the owner says to:

```bash
git push --force-with-lease origin <branch>
```

The old commits can stay reachable by SHA on GitHub through the PR's refs, so a
force push does not purge something committed by mistake, such as a large file
or a secret. Rotate a secret; a large file is only removed from the branch.
