#!/usr/bin/env python3
"""Small, typed Git operations used by Harness V2."""

from __future__ import annotations

import json
import os
import subprocess
import tempfile
from pathlib import Path
from typing import Any, Dict, List, Mapping, Optional, Sequence, Tuple

from model import ContractError, canonical_bytes


ZERO_SHA = "0" * 40


class GitError(RuntimeError):
    """Raised when an exact Git operation fails."""


def run_git(
    root: Path,
    *arguments: str,
    input_bytes: Optional[bytes] = None,
    check: bool = True,
    environment: Optional[Mapping[str, str]] = None,
) -> subprocess.CompletedProcess:
    env = os.environ.copy()
    if environment:
        env.update(environment)
    result = subprocess.run(
        ["git", "-C", str(root), *arguments],
        input=input_bytes,
        check=False,
        capture_output=True,
        env=env,
    )
    if check and result.returncode != 0:
        stderr = result.stderr.decode("utf-8", errors="replace").strip()
        raise GitError(
            "git {} failed ({}): {}".format(
                " ".join(arguments), result.returncode, stderr
            )
        )
    return result


def git_text(
    root: Path,
    *arguments: str,
    check: bool = True,
    input_bytes: Optional[bytes] = None,
    environment: Optional[Mapping[str, str]] = None,
) -> str:
    result = run_git(
        root,
        *arguments,
        check=check,
        input_bytes=input_bytes,
        environment=environment,
    )
    return result.stdout.decode("utf-8", errors="strict").strip()


def repository_root(path: Path) -> Path:
    return Path(git_text(path, "rev-parse", "--show-toplevel")).resolve()


def common_git_dir(root: Path) -> Path:
    value = Path(git_text(root, "rev-parse", "--git-common-dir"))
    if not value.is_absolute():
        value = root / value
    return value.resolve()


def object_id(root: Path, revision_path: str) -> str:
    value = git_text(root, "rev-parse", "--verify", revision_path)
    if len(value) != 40:
        raise GitError("{} did not resolve to a SHA-1 object".format(revision_path))
    return value


def ref_sha(root: Path, ref: str) -> Optional[str]:
    result = run_git(root, "rev-parse", "--verify", ref, check=False)
    if result.returncode != 0:
        return None
    value = result.stdout.decode("ascii").strip()
    if len(value) != 40:
        raise GitError("ref {} resolved to an invalid SHA".format(ref))
    return value


def list_refs(root: Path, prefix: str) -> Dict[str, str]:
    output = git_text(
        root,
        "for-each-ref",
        "--format=%(refname) %(objectname)",
        prefix,
    )
    result: Dict[str, str] = {}
    for line in output.splitlines():
        if not line:
            continue
        ref, sha = line.split(" ", 1)
        result[ref] = sha
    return result


def commit_json(
    root: Path,
    payload: Mapping[str, Any],
    message: str,
    parent: Optional[str] = None,
    filename: str = "event.json",
) -> str:
    if "/" in filename or filename in {"", ".", ".."}:
        raise GitError("JSON commit filename must be one basename")
    blob = git_text(
        root,
        "hash-object",
        "-w",
        "--stdin",
        input_bytes=canonical_bytes(payload) + b"\n",
    )
    tree_input = "100644 blob {}\t{}\n".format(blob, filename).encode("utf-8")
    tree = git_text(root, "mktree", input_bytes=tree_input)
    arguments = ["commit-tree", tree]
    if parent is not None:
        arguments.extend(["-p", parent])
    commit = git_text(
        root,
        *arguments,
        input_bytes=(message.rstrip() + "\n").encode("utf-8"),
    )
    if len(commit) != 40:
        raise GitError("commit-tree returned an invalid SHA")
    return commit


def update_ref_cas(
    root: Path, ref: str, new_sha: str, expected_old: Optional[str]
) -> None:
    expected = expected_old or ZERO_SHA
    result = run_git(
        root,
        "update-ref",
        ref,
        new_sha,
        expected,
        check=False,
    )
    if result.returncode != 0:
        raise GitError("compare-and-swap failed for {}".format(ref))


def delete_ref_cas(root: Path, ref: str, expected_old: str) -> bool:
    current = ref_sha(root, ref)
    if current is None:
        return False
    if current != expected_old:
        raise GitError("compare-and-swap failed for {}".format(ref))
    result = run_git(
        root,
        "update-ref",
        "-d",
        ref,
        expected_old,
        check=False,
    )
    if result.returncode != 0:
        raise GitError("compare-and-swap failed for {}".format(ref))
    return True


def append_json_ref(
    root: Path,
    ref: str,
    payload: Mapping[str, Any],
    message: str,
    expected_old: Optional[str],
    filename: str = "event.json",
) -> str:
    current = ref_sha(root, ref)
    if current != expected_old:
        raise GitError(
            "stale ref {}: expected {}, found {}".format(
                ref, expected_old or "<missing>", current or "<missing>"
            )
        )
    commit = commit_json(
        root,
        payload,
        message,
        parent=current,
        filename=filename,
    )
    update_ref_cas(root, ref, commit, current)
    return commit


def read_json_object(
    root: Path, commit: str, filename: str = "event.json"
) -> Dict[str, Any]:
    result = run_git(root, "show", "{}:{}".format(commit, filename), check=False)
    if result.returncode != 0:
        raise GitError("{} does not contain {}".format(commit, filename))
    try:
        value = json.loads(
            result.stdout.decode("utf-8"),
            object_pairs_hook=_reject_duplicate_pairs,
        )
    except (UnicodeDecodeError, json.JSONDecodeError, ContractError) as error:
        raise GitError("{} contains invalid JSON: {}".format(commit, error)) from error
    if not isinstance(value, dict):
        raise GitError("{} JSON must be an object".format(commit))
    return value


def _reject_duplicate_pairs(pairs: Sequence[Tuple[str, Any]]) -> Dict[str, Any]:
    result: Dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ContractError("duplicate JSON key: {}".format(key))
        result[key] = value
    return result


def first_parent_history(root: Path, ref: str) -> List[str]:
    output = git_text(root, "rev-list", "--first-parent", "--reverse", ref)
    return [line for line in output.splitlines() if line]


def commit_parents(root: Path, commit: str) -> List[str]:
    value = git_text(root, "show", "-s", "--format=%P", commit)
    return value.split() if value else []


def current_tree(root: Path, revision: str = "HEAD") -> str:
    return object_id(root, "{}^{{tree}}".format(revision))


def is_clean(root: Path) -> bool:
    return git_text(root, "status", "--porcelain=v1") == ""


def is_ancestor(root: Path, ancestor: str, descendant: str) -> bool:
    return (
        run_git(
            root,
            "merge-base",
            "--is-ancestor",
            ancestor,
            descendant,
            check=False,
        ).returncode
        == 0
    )


def commit_range(root: Path, base: str, head: str) -> List[str]:
    output = git_text(root, "rev-list", "--reverse", "{}..{}".format(base, head))
    return [line for line in output.splitlines() if line]


def changed_paths(root: Path, base: str, head: str) -> List[str]:
    output = git_text(
        root,
        "diff",
        "--name-only",
        "--no-renames",
        "{}..{}".format(base, head),
    )
    return [line for line in output.splitlines() if line]


def commit_changed_paths(root: Path, commit: str) -> List[str]:
    output = git_text(
        root,
        "diff-tree",
        "--root",
        "--no-commit-id",
        "--name-only",
        "-r",
        "--no-renames",
        commit,
    )
    return [line for line in output.splitlines() if line]


def aggregate_patch_id(
    root: Path,
    base: str,
    head: str,
    excluded_paths: Optional[Sequence[str]] = None,
) -> str:
    arguments = ["diff", "--binary", base, head]
    if excluded_paths:
        arguments.extend(
            ["--", ".", *(":(exclude){}".format(p) for p in excluded_paths)]
        )
    diff = run_git(root, *arguments).stdout
    result = subprocess.run(
        ["git", "patch-id", "--stable"],
        input=diff,
        check=False,
        capture_output=True,
    )
    if result.returncode != 0:
        raise GitError("git patch-id failed")
    output = result.stdout.decode("ascii").strip()
    if not output:
        raise GitError("payload patch is empty")
    return output.split()[0]


def add_worktree(root: Path, path: Path, branch: str, start_point: str) -> None:
    if path.exists():
        raise GitError("worktree path already exists: {}".format(path))
    if ref_sha(root, "refs/heads/{}".format(branch)) is not None:
        raise GitError("branch already exists: {}".format(branch))
    run_git(root, "worktree", "add", "-b", branch, str(path), start_point)


def remove_worktree(root: Path, path: Path) -> None:
    if not path.exists():
        return
    if not is_clean(path):
        raise GitError("worktree is dirty: {}".format(path))
    run_git(root, "worktree", "remove", str(path))


def remote_ref_sha(root: Path, remote: str, ref: str) -> Optional[str]:
    output = git_text(root, "ls-remote", "--heads", remote, ref)
    if not output:
        return None
    rows = output.splitlines()
    if len(rows) != 1:
        raise GitError("remote ref {} is ambiguous".format(ref))
    sha, actual_ref = rows[0].split()
    if actual_ref != ref or len(sha) != 40:
        raise GitError("remote ref {} returned invalid data".format(ref))
    return sha


def fetch_remote_object(root: Path, remote: str, ref: str) -> str:
    remote_sha = remote_ref_sha(root, remote, ref)
    if remote_sha is None:
        raise GitError("remote ref {} is missing".format(ref))
    present = run_git(
        root,
        "cat-file",
        "-e",
        "{}^{{commit}}".format(remote_sha),
        check=False,
    )
    if present.returncode != 0:
        result = run_git(
            root,
            "fetch",
            "--no-tags",
            remote,
            ref,
            check=False,
        )
        if result.returncode != 0:
            raise GitError("cannot fetch remote object for {}".format(ref))
    return remote_sha


def fetch_immutable_ref(root: Path, remote: str, ref: str) -> str:
    remote_sha = remote_ref_sha(root, remote, ref)
    if remote_sha is None:
        raise GitError("remote immutable ref {} is missing".format(ref))
    local_sha = ref_sha(root, ref)
    if local_sha is not None:
        if local_sha != remote_sha:
            raise GitError(
                "immutable ref {} differs between local and remote".format(ref)
            )
        return local_sha
    result = run_git(
        root,
        "fetch",
        "--no-tags",
        remote,
        "{}:{}".format(ref, ref),
        check=False,
    )
    if result.returncode != 0 or ref_sha(root, ref) != remote_sha:
        raise GitError("cannot fetch immutable ref {}".format(ref))
    return remote_sha


def push_ref_cas(
    root: Path,
    remote: str,
    local_sha: str,
    remote_ref: str,
    expected_remote: Optional[str],
) -> None:
    lease = "--force-with-lease={}:{}".format(remote_ref, expected_remote or "")
    result = run_git(
        root,
        "push",
        lease,
        remote,
        "{}:{}".format(local_sha, remote_ref),
        check=False,
    )
    if result.returncode != 0:
        raise GitError("remote compare-and-swap failed for {}".format(remote_ref))


def delete_remote_ref_cas(
    root: Path,
    remote: str,
    remote_ref: str,
    expected_remote: str,
) -> bool:
    current = remote_ref_sha(root, remote, remote_ref)
    if current is None:
        return False
    if current != expected_remote:
        raise GitError("remote compare-and-swap failed for {}".format(remote_ref))
    lease = "--force-with-lease={}:{}".format(remote_ref, expected_remote)
    result = run_git(
        root,
        "push",
        lease,
        remote,
        ":{}".format(remote_ref),
        check=False,
    )
    if result.returncode != 0:
        raise GitError("remote compare-and-swap failed for {}".format(remote_ref))
    if remote_ref_sha(root, remote, remote_ref) is not None:
        raise GitError("remote ref deletion was not durable for {}".format(remote_ref))
    return True


def list_remote_refs(root: Path, remote: str, prefix: str) -> Dict[str, str]:
    output = git_text(root, "ls-remote", "--heads", remote, "{}*".format(prefix))
    result: Dict[str, str] = {}
    for line in output.splitlines():
        if not line:
            continue
        sha, ref = line.split()
        if not ref.startswith(prefix) or len(sha) != 40:
            raise GitError("remote ref inventory is invalid for {}".format(prefix))
        result[ref] = sha
    return result


def atomic_write_json(path: Path, value: Mapping[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        mode="wb",
        dir=str(path.parent),
        prefix=".{}".format(path.name),
        delete=False,
    ) as output:
        output.write(canonical_bytes(value) + b"\n")
        temporary = Path(output.name)
    temporary.replace(path)
