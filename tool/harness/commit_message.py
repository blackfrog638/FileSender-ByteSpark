#!/usr/bin/env python3

"""Validate repository commit messages and identities."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import NamedTuple


ROOT = Path(__file__).resolve().parents[2]
POLICY_PATH = "docs/commit-policy.md"
IDENTITY_POLICY_PATH = ".agents/commit-identity.json"
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
GIT_IDENT_PATTERN = re.compile(
    r"^(?P<name>[^<>\r\n]+) <(?P<email>[^<>\s]+)> "
    r"[0-9]+ [+-][0-9]{4}$"
)


class CommitMessageError(ValueError):
    pass


class CommitIdentity(NamedTuple):
    name: str
    email: str

    def display(self) -> str:
        return f"{self.name} <{self.email}>"


class IdentityPolicy(NamedTuple):
    schema_version: int
    identity: CommitIdentity


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
        encoding="utf-8",
        errors="strict",
    )


def parse_identity_policy_document(source: str) -> IdentityPolicy:
    try:
        document = json.loads(source)
    except json.JSONDecodeError as error:
        raise CommitMessageError(
            f"{IDENTITY_POLICY_PATH} is not valid JSON: {error}"
        ) from error
    if not isinstance(document, dict):
        raise CommitMessageError(
            f"{IDENTITY_POLICY_PATH} must contain a JSON object"
        )
    schema_version = document.get("schema_version")
    if type(schema_version) is not int or schema_version not in (1, 2):
        raise CommitMessageError(
            f"{IDENTITY_POLICY_PATH}.schema_version must be 1 or 2"
        )
    if schema_version == 1:
        expected_fields = {"schema_version", "name", "email"}
    else:
        expected_fields = {
            "schema_version",
            "name",
            "email",
            "immutable",
        }
    if set(document) != expected_fields:
        raise CommitMessageError(
            f"{IDENTITY_POLICY_PATH} must contain exactly: "
            + ", ".join(sorted(expected_fields))
        )
    if schema_version == 2 and document["immutable"] is not True:
        raise CommitMessageError(
            f"{IDENTITY_POLICY_PATH}.immutable must be true for schema 2"
        )

    name = document["name"]
    email = document["email"]
    if (
        not isinstance(name, str)
        or not name
        or name != name.strip()
        or any(character in name for character in "<>\r\n")
    ):
        raise CommitMessageError(
            f"{IDENTITY_POLICY_PATH}.name must be a canonical Git name"
        )
    if (
        not isinstance(email, str)
        or not email
        or email != email.strip()
        or any(character.isspace() for character in email)
        or any(character in email for character in "<>")
    ):
        raise CommitMessageError(
            f"{IDENTITY_POLICY_PATH}.email must be a canonical Git email"
        )
    return IdentityPolicy(
        schema_version=schema_version,
        identity=CommitIdentity(name=name, email=email),
    )


def parse_identity_policy(source: str) -> CommitIdentity:
    return parse_identity_policy_document(source).identity


def load_identity_policy_document(
    root: Path,
    commit: str | None = None,
) -> IdentityPolicy:
    if commit is None:
        try:
            source = (root / IDENTITY_POLICY_PATH).read_text(encoding="utf-8")
        except OSError as error:
            raise CommitMessageError(
                f"cannot read {IDENTITY_POLICY_PATH}: {error}"
            ) from error
    else:
        result = git(
            root,
            "show",
            f"{commit}:{IDENTITY_POLICY_PATH}",
            check=False,
        )
        if result.returncode != 0:
            raise CommitMessageError(
                f"{commit[:12]} cannot read {IDENTITY_POLICY_PATH}"
            )
        source = result.stdout
    return parse_identity_policy_document(source)


def load_identity_policy(
    root: Path,
    commit: str | None = None,
) -> CommitIdentity:
    return load_identity_policy_document(root, commit).identity


def load_effective_identity_policy(root: Path) -> CommitIdentity:
    committed = git(
        root,
        "show",
        f"HEAD:{IDENTITY_POLICY_PATH}",
        check=False,
    )
    if committed.returncode == 0:
        policy = parse_identity_policy_document(committed.stdout)
        if policy.schema_version == 2:
            return policy.identity
    return load_identity_policy(root)


def parse_git_ident(value: str, label: str) -> CommitIdentity:
    match = GIT_IDENT_PATTERN.fullmatch(value.rstrip("\n"))
    if match is None:
        raise CommitMessageError(f"cannot parse {label} identity from Git")
    return CommitIdentity(
        name=match.group("name"),
        email=match.group("email"),
    )


def validate_identities(
    expected: CommitIdentity,
    author: CommitIdentity,
    committer: CommitIdentity,
) -> list[str]:
    errors: list[str] = []
    if author != expected:
        errors.append(
            "author identity must be "
            f"{expected.display()}; got {author.display()}"
        )
    if committer != expected:
        errors.append(
            "committer identity must be "
            f"{expected.display()}; got {committer.display()}"
        )
    return errors


def current_identities(root: Path) -> tuple[CommitIdentity, CommitIdentity]:
    author = parse_git_ident(
        git(root, "var", "GIT_AUTHOR_IDENT").stdout,
        "author",
    )
    committer = parse_git_ident(
        git(root, "var", "GIT_COMMITTER_IDENT").stdout,
        "committer",
    )
    return author, committer


def commit_identities(
    root: Path,
    commit: str,
) -> tuple[CommitIdentity, CommitIdentity]:
    fields = git(
        root,
        "show",
        "-s",
        "--format=%an%x00%ae%x00%cn%x00%ce",
        commit,
    ).stdout.rstrip("\n").split("\0")
    if len(fields) != 4:
        raise CommitMessageError(
            f"cannot read author and committer identity for {commit[:12]}"
        )
    return (
        CommitIdentity(name=fields[0], email=fields[1]),
        CommitIdentity(name=fields[2], email=fields[3]),
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


def path_activations(root: Path, path: str) -> list[str]:
    result = git(
        root,
        "log",
        "--all",
        "--format=%H",
        "--diff-filter=A",
        "--",
        path,
    )
    return [line for line in result.stdout.splitlines() if line]


def policy_activations(root: Path) -> list[str]:
    return path_activations(root, POLICY_PATH)


def is_ancestor(root: Path, ancestor: str, commit: str) -> bool:
    return (
        git(
            root,
            "merge-base",
            "--is-ancestor",
            ancestor,
            commit,
            check=False,
        ).returncode
        == 0
    )


def policy_applies(root: Path, commit: str, activations: list[str]) -> bool:
    return any(
        is_ancestor(root, activation, commit)
        for activation in activations
    )


def immutable_identity_activations(root: Path) -> list[str]:
    changes = git(
        root,
        "log",
        "--all",
        "--format=%H",
        "--",
        IDENTITY_POLICY_PATH,
    )
    activations: list[str] = []
    for commit in changes.stdout.splitlines():
        try:
            policy = load_identity_policy_document(root, commit)
        except CommitMessageError:
            continue
        if policy.schema_version == 2:
            activations.append(commit)
    return activations


def governing_activation(
    root: Path,
    commit: str,
    activations: list[str],
) -> str | None:
    applicable = [
        activation
        for activation in activations
        if is_ancestor(root, activation, commit)
    ]
    if not applicable:
        return None
    roots = [
        activation
        for activation in applicable
        if all(
            is_ancestor(root, activation, descendant)
            for descendant in applicable
        )
    ]
    if len(roots) != 1:
        raise CommitMessageError(
            f"{commit[:12]} has ambiguous immutable identity activations"
        )
    return roots[0]


def commit_changes_path(root: Path, commit: str, path: str) -> bool:
    result = git(
        root,
        "diff-tree",
        "--no-commit-id",
        "--name-only",
        "-r",
        "--root",
        commit,
        "--",
        path,
    )
    return bool(result.stdout.strip())


def staged_identity_policy_changed(root: Path) -> bool:
    committed = git(
        root,
        "show",
        f"HEAD:{IDENTITY_POLICY_PATH}",
        check=False,
    )
    if committed.returncode != 0:
        return False
    policy = parse_identity_policy_document(committed.stdout)
    if policy.schema_version != 2:
        return False
    staged = git(
        root,
        "diff",
        "--cached",
        "--quiet",
        "HEAD",
        "--",
        IDENTITY_POLICY_PATH,
        check=False,
    )
    if staged.returncode not in (0, 1):
        raise CommitMessageError(
            f"cannot inspect staged {IDENTITY_POLICY_PATH}"
        )
    return staged.returncode == 1


def validate_range(root: Path, base: str, head: str) -> tuple[int, list[str]]:
    commits = git(root, "rev-list", "--reverse", f"{base}..{head}")
    message_activations = policy_activations(root)
    identity_activations = path_activations(root, IDENTITY_POLICY_PATH)
    immutable_activations = immutable_identity_activations(root)
    checked = 0
    failures: list[str] = []
    for commit in commits.stdout.splitlines():
        check_message_policy = policy_applies(
            root,
            commit,
            message_activations,
        )
        check_identity_policy = policy_applies(
            root,
            commit,
            identity_activations,
        )
        immutable_activation: str | None = None
        immutable_activation_error = ""
        try:
            immutable_activation = governing_activation(
                root,
                commit,
                immutable_activations,
            )
        except CommitMessageError as error:
            immutable_activation_error = str(error)
        check_identity_policy = bool(
            check_identity_policy
            or immutable_activation
            or immutable_activation_error
        )
        if not check_message_policy and not check_identity_policy:
            continue
        checked += 1
        message = git(root, "show", "-s", "--format=%B", commit).stdout
        errors: list[str] = []
        if check_message_policy:
            errors.extend(validate_message(message))
        if check_identity_policy:
            if immutable_activation_error:
                errors.append(immutable_activation_error)
            else:
                policy_commit = immutable_activation or commit
                try:
                    expected = load_identity_policy(root, policy_commit)
                    author, committer = commit_identities(root, commit)
                except CommitMessageError as error:
                    errors.append(str(error))
                else:
                    errors.extend(
                        validate_identities(expected, author, committer)
                    )
                    if (
                        immutable_activation is not None
                        and commit != immutable_activation
                        and commit_changes_path(
                            root,
                            commit,
                            IDENTITY_POLICY_PATH,
                        )
                    ):
                        errors.append(
                            f"{IDENTITY_POLICY_PATH} is immutable after "
                            f"schema 2 activation "
                            f"{immutable_activation[:12]}"
                        )
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


def check_current_identity(root: Path) -> None:
    expected = load_effective_identity_policy(root)
    author, committer = current_identities(root)
    errors = validate_identities(expected, author, committer)
    if staged_identity_policy_changed(root):
        errors.append(
            f"{IDENTITY_POLICY_PATH} is immutable after schema 2 activation"
        )
    if errors:
        raise CommitMessageError("\n".join(f"- {error}" for error in errors))


def configure_identity(root: Path) -> None:
    expected = load_effective_identity_policy(root)
    git(root, "config", "--local", "user.name", expected.name)
    git(root, "config", "--local", "user.email", expected.email)


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

    hook_parser = subparsers.add_parser("hook")
    hook_parser.add_argument("path", type=Path)
    hook_parser.add_argument("--root", type=Path, default=ROOT)

    range_parser = subparsers.add_parser("range")
    range_parser.add_argument("base")
    range_parser.add_argument("head")
    range_parser.add_argument("--root", type=Path, default=ROOT)

    configure_parser = subparsers.add_parser("configure")
    configure_parser.add_argument("--root", type=Path, default=ROOT)

    args = parser.parse_args()
    try:
        if args.command == "message":
            check_message(args.path)
        elif args.command == "hook":
            check_message(args.path)
            check_current_identity(args.root.resolve())
        elif args.command == "range":
            check_range(args.root.resolve(), args.base, args.head)
        else:
            configure_identity(args.root.resolve())
    except (CommitMessageError, OSError, subprocess.CalledProcessError) as error:
        print(f"Commit policy error:\n{error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
