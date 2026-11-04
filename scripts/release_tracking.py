#!/usr/bin/env python3
"""Applies or removes the "in-dev" release-tracking label on whichever
issues a merged PR's body closes, using the same closing-keyword syntax
GitHub itself recognizes (Closes/Fixes/Resolves #N). Used by
.github/workflows/release-tracking.yml on a PR merge into dev (label) or
main (unlabel, since GitHub's own mechanism does the closing on main).

Reads the PR body from the PR_BODY environment variable rather than a CLI
argument, so a multi-line body with quotes never has to survive shell
argument passing.

Usage: release_tracking.py {label,unlabel} --label <name>
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys

# The closing keywords GitHub itself recognizes in a PR body
# (docs.github.com), case-insensitive, followed by #<number>. Deliberately
# does not match "owner/repo#N": every reference in this project's PR
# bodies so far has been to an issue in this same repo.
_CLOSES_RE = re.compile(
    r"\b(?:close[sd]?|fix(?:e[sd])?|resolve[sd]?)\b\s*:?\s*#(\d+)",
    re.IGNORECASE,
)


def closed_issue_numbers(body: str) -> list[int]:
    return sorted({int(n) for n in _CLOSES_RE.findall(body)})


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["label", "unlabel"])
    parser.add_argument("--label", required=True)
    args = parser.parse_args()

    body = os.environ.get("PR_BODY", "")
    issues = closed_issue_numbers(body)
    if not issues:
        print("no Closes/Fixes/Resolves #N reference found in the PR body, nothing to do")
        return 0

    flag = "--add-label" if args.action == "label" else "--remove-label"
    failures = []
    for number in issues:
        result = subprocess.run(
            ["gh", "issue", "edit", str(number), flag, args.label],
            check=False,
            capture_output=True,
            text=True,
        )
        if result.returncode == 0:
            print(f"#{number}: {args.action}ed '{args.label}'")
        else:
            # An issue number the regex picked up that does not exist, or
            # already lacks the label being removed, must not fail the
            # whole run over the other issues in the same PR body.
            failures.append(number)
            print(f"#{number}: {flag} '{args.label}' failed: {result.stderr.strip()}",
                  file=sys.stderr)

    if failures:
        print(f"warning: {len(failures)} of {len(issues)} issue(s) could not be updated",
              file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
