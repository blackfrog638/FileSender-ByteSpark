#!/usr/bin/env python3

"""Validate meaningful Conventional Commit messages."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
POLICY_PATH = "docs/commit-policy.md"
ALLOWED_TYPES = {
    "feat",
    "fix",
    "docs",
    "style",
    "refactor",
    "perf",
    "test",
    "build",
    "ci",
    "chore",
    "revert",
}
HEADER_PATTERN = re.compile(
    r"^(?P<type>[a-z]+)\((?P<scope>[a-z0-9][a-z0-9.-]*)\): "
    r"(?P<summary>.+)$"
)
TASK_PATTERN = re.compile(r"\bXT-[0-9]{3,}\b", flags=re.IGNORECASE)
TASK_TRAILER_PATTERN = re.compile(r"^Xnn-Task: XT-[0-9]{3,}$")
TRAILER_PATTERN = re.compile(r"^[A-Za-z][A-Za-z0-9-]*: \S.*$")
VAGUE_SUMMARIES = (
    re.compile(
        r"^(?:changes?|misc|more changes|stuff|updates?|work in progress|"
        r"wip|tmp|temp|checkpoint)$"
    ),
    re.compile(r"^(?:fix|update|change|adjust|modify) (?:issue|files?|code|stuff)$"),
    re.compile(r"^address (?:feedback|comments?|review comments?)$"),
    re.compile(r"^(?:plan|claim|deliver|accept|move|record) XT-[0-9]{3,}$"),
)


class CommitMessageError(ValueError):
    pass


def git(
    root: Path,
    *args: str,
    check: bool = True,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["git", "-C", str(root), *args],
        check=check,
        capture_output=True,
        text=True,
    )


def validate_subject(subject: str) -> list[str]:
    errors: list[str] = []
    if len(subject) > 72:
        errors.append("subject must not exceed 72 characters")
    match = HEADER_PATTERN.fullmatch(subject)
    if match is None:
        errors.append("subject must match type(scope): summary")
        return errors

    commit_type = match.group("type")
    summary = match.group("summary")
    if commit_type not in ALLOWED_TYPES:
        errors.append(
            "type must be one of: " + ", ".join(sorted(ALLOWED_TYPES))
        )
    if len(summary) < 12:
        errors.append("summary must contain at least 12 characters")
    if not summary[0].islower():
        errors.append("summary must start with a lowercase imperative verb")
    if summary.endswith((".", "!", "?")):
        errors.append("summary must not end with punctuation")
    if TASK_PATTERN.search(summary):
        errors.append("task IDs belong in an Xnn-Task trailer, not the subject")
    if any(pattern.fullmatch(summary) for pattern in VAGUE_SUMMARIES):
        errors.append("summary is vague and does not describe repository impact")
    return errors


def validate_message(message: str) -> list[str]:
    lines = message.rstrip("\n").splitlines()
    if not lines or not lines[0]:
        return ["commit message is empty"]

    errors = validate_subject(lines[0])
    if len(lines) > 1 and lines[1]:
        errors.append("subject and body must be separated by a blank line")

    task_trailers = 0
    trailer_started = False
    for index, line in enumerate(lines[2:], start=3):
        if TASK_PATTERN.search(line):
            if not TASK_TRAILER_PATTERN.fullmatch(line):
                errors.append(
                    f"line {index}: task IDs are only allowed in "
                    "an Xnn-Task trailer"
                )
            else:
                task_trailers += 1
        if line.startswith("Xnn-") and not TRAILER_PATTERN.fullmatch(line):
            errors.append(f"line {index}: malformed Xnn trailer")
        if line.startswith("Xnn-"):
            trailer_started = True
        elif trailer_started and line:
            errors.append(
                f"line {index}: Xnn trailers must be the final contiguous block"
            )
        elif trailer_started and not line:
            errors.append("Xnn trailers must not contain blank lines")

    if task_trailers > 1:
        errors.append("commit message contains multiple Xnn-Task trailers")
    return errors


def policy_activations(root: Path) -> list[str]:
    result = git(
        root,
        "log",
        "--all",
        "--format=%H",
        "--diff-filter=A",
        "--",
        POLICY_PATH,
    )
    return [line for line in result.stdout.splitlines() if line]


def policy_applies(root: Path, commit: str, activations: list[str]) -> bool:
    return any(
        git(
            root,
            "merge-base",
            "--is-ancestor",
            activation,
            commit,
            check=False,
        ).returncode
        == 0
        for activation in activations
    )


def validate_range(root: Path, base: str, head: str) -> tuple[int, list[str]]:
    commits = git(root, "rev-list", "--reverse", f"{base}..{head}")
    activations = policy_activations(root)
    checked = 0
    failures: list[str] = []
    for commit in commits.stdout.splitlines():
        if not policy_applies(root, commit, activations):
            continue
        checked += 1
        message = git(root, "show", "-s", "--format=%B", commit).stdout
        errors = validate_message(message)
        if errors:
            subject = message.splitlines()[0] if message.splitlines() else ""
            failures.append(f"{commit[:12]} {subject}")
            failures.extend(f"  - {error}" for error in errors)
    return checked, failures


def check_message(path: Path) -> None:
    message = "\n".join(
        line
        for line in path.read_text(encoding="utf-8").splitlines()
        if not line.startswith("#")
    )
    errors = validate_message(message)
    if errors:
        raise CommitMessageError("\n".join(f"- {error}" for error in errors))


def check_range(root: Path, base: str, head: str) -> None:
    checked, failures = validate_range(root, base, head)
    if failures:
        raise CommitMessageError(
            "Non-compliant commits:\n" + "\n".join(failures)
        )
    if checked:
        print(f"Commit policy passed for {checked} commit(s).")
    else:
        print("Commit policy not active in the selected range; nothing checked.")


def main() -> int:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)

    message_parser = subparsers.add_parser("message")
    message_parser.add_argument("path", type=Path)

    range_parser = subparsers.add_parser("range")
    range_parser.add_argument("base")
    range_parser.add_argument("head")
    range_parser.add_argument("--root", type=Path, default=ROOT)

    args = parser.parse_args()
    try:
        if args.command == "message":
            check_message(args.path)
        else:
            check_range(args.root.resolve(), args.base, args.head)
    except (CommitMessageError, OSError, subprocess.CalledProcessError) as error:
        print(f"Commit message error:\n{error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
