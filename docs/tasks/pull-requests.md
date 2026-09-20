---
aliases: [pr, pull request, gh pr, force push]
---

# Pull requests

How to open, update and review a pull request in this repository, and the one
`gh` failure that looks like success.

Opening or merging a PR, and pushing, need the owner's explicit go-ahead each
time; a yes to one does not carry over to the next. Branches are `feature/...`
and PRs target `dev`, where they are squash-merged. `dev` goes into `main` only
for a release, as a merge commit, when the owner asks.

One exception is configured, not assumed. `.claude/workflows.json` sets
`git.askBeforePush` and `git.askBeforePR` to `false`, so a green
`implement-plan` run pushes its branch and opens the PR into the branch it was
cut from, in the same session. That is deliberate: coming back to a cold prompt
cache just to type "open the PR" costs a full context. It never force-pushes and
never merges, and it does nothing after a failed or incomplete run. Everything
else here, including interactive sessions, still asks first.

## Before opening a PR

The push-time hook runs the full suite under ASan+UBSan and TSan
(`decisions/0005`). It exists only after `pre-commit install --hook-type
pre-push`, and a fresh clone has none, so a push can succeed without the suite
having run. Check `ls .git/hooks/pre-push` before trusting it.

Run the same hook without pushing:

```bash
pre-commit run --all-files --hook-stage pre-push
```

An `implement-plan` run does this itself: the `sanitizers` check in
`.claude/workflows.json` has no `scopedCmd`, so it runs once at the end, before
the push, and a finding fails the run in the session that has the context to fix
it. An interactive session has no such check, so run the command above before
asking to push.

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

Compare against the merge-base with three dots, so commits that landed on the
base branch since the branch was cut do not show up as removals. The base is
`dev` for a feature branch, and always the `origin/` ref: a local `dev` or
`main` can lag the remote and inflate the diff with work that is already merged.

```bash
git diff --stat origin/dev...HEAD     # the whole branch, per file
git log --stat origin/dev..HEAD       # commit by commit
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
