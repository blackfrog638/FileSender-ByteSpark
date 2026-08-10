#!/usr/bin/env python3

"""Wait for an exact GitHub Actions workflow run without exposing credentials."""

from __future__ import annotations

import argparse
import base64
import json
import re
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from typing import Any


SHA_PATTERN = re.compile(r"^[0-9a-f]{40}$")
BRANCH_PATTERN = re.compile(r"^ci/XT-[0-9]{3,}$")
WORKFLOW_PATTERN = re.compile(r"^[A-Za-z0-9._-]+$")
HTTPS_REMOTE_PATTERN = re.compile(
    r"^https://github\.com/([^/]+)/([^/]+?)(?:\.git)?$"
)
SSH_REMOTE_PATTERN = re.compile(
    r"^(?:ssh://git@github\.com/|git@github\.com:)"
    r"([^/]+)/([^/]+?)(?:\.git)?$"
)
RUN_URL_PATTERN = re.compile(
    r"^https://github\.com/([^/]+)/([^/]+)/actions/runs/([1-9][0-9]*)$"
)
PENDING_STATES = {"queued", "in_progress", "pending", "requested", "waiting"}


class GitHubCIError(RuntimeError):
    pass


@dataclass(frozen=True)
class Repository:
    owner: str
    name: str


@dataclass(frozen=True)
class WorkflowRun:
    run_id: int
    status: str
    conclusion: str | None
    html_url: str


def parse_remote_url(value: str) -> Repository:
    match = HTTPS_REMOTE_PATTERN.fullmatch(value) or SSH_REMOTE_PATTERN.fullmatch(
        value
    )
    if match is None:
        raise GitHubCIError(
            "origin must identify a github.com owner/repository remote"
        )
    owner, name = match.groups()
    if not owner or not name:
        raise GitHubCIError("GitHub owner and repository must be non-empty")
    return Repository(owner=owner, name=name)


def git_text(root: Path, *args: str, input_text: str | None = None) -> str:
    result = subprocess.run(
        ["git", "-C", str(root), *args],
        check=False,
        input=input_text,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        raise GitHubCIError(f"git {' '.join(args)} failed: {detail}")
    return result.stdout.strip()


def load_repository(root: Path, remote: str) -> Repository:
    return parse_remote_url(git_text(root, "remote", "get-url", remote))


def load_credentials(root: Path) -> tuple[str, str]:
    output = git_text(
        root,
        "credential",
        "fill",
        input_text="protocol=https\nhost=github.com\n\n",
    )
    values: dict[str, str] = {}
    for line in output.splitlines():
        key, separator, value = line.partition("=")
        if separator:
            values[key] = value
    username = values.get("username", "")
    password = values.get("password", "")
    if not username or not password:
        raise GitHubCIError(
            "Git credential helper returned no github.com credentials"
        )
    return username, password


class GitHubClient:
    def __init__(
        self,
        repository: Repository,
        username: str,
        password: str,
    ) -> None:
        self.repository = repository
        encoded = base64.b64encode(
            f"{username}:{password}".encode("utf-8")
        ).decode("ascii")
        self.headers = {
            "Accept": "application/vnd.github+json",
            "Authorization": f"Basic {encoded}",
            "User-Agent": "xnn-transfer-harness",
            "X-GitHub-Api-Version": "2022-11-28",
        }

    def workflow_runs(self, workflow: str, branch: str) -> dict[str, Any]:
        query = urllib.parse.urlencode(
            {"branch": branch, "event": "push", "per_page": "100"}
        )
        owner = urllib.parse.quote(self.repository.owner, safe="")
        name = urllib.parse.quote(self.repository.name, safe="")
        workflow_id = urllib.parse.quote(workflow, safe="")
        url = (
            f"https://api.github.com/repos/{owner}/{name}/actions/workflows/"
            f"{workflow_id}/runs?{query}"
        )
        request = urllib.request.Request(url, headers=self.headers)
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                payload = json.load(response)
        except urllib.error.HTTPError as error:
            raise GitHubCIError(
                f"GitHub Actions API returned HTTP {error.code}"
            ) from error
        except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as error:
            raise GitHubCIError(f"GitHub Actions API request failed: {error}") from error
        if not isinstance(payload, dict):
            raise GitHubCIError("GitHub Actions API returned a non-object payload")
        return payload


def select_run(
    payload: dict[str, Any],
    repository: Repository,
    branch: str,
    sha: str,
) -> WorkflowRun | None:
    raw_runs = payload.get("workflow_runs")
    if not isinstance(raw_runs, list):
        raise GitHubCIError("GitHub Actions response has no workflow_runs array")
    matches: list[WorkflowRun] = []
    for raw in raw_runs:
        if not isinstance(raw, dict):
            continue
        if (
            raw.get("event") != "push"
            or raw.get("head_branch") != branch
            or raw.get("head_sha") != sha
        ):
            continue
        run_id = raw.get("id")
        status = raw.get("status")
        conclusion = raw.get("conclusion")
        html_url = raw.get("html_url")
        if (
            not isinstance(run_id, int)
            or not isinstance(status, str)
            or (conclusion is not None and not isinstance(conclusion, str))
            or not isinstance(html_url, str)
        ):
            raise GitHubCIError("Matching workflow run has invalid fields")
        url_match = RUN_URL_PATTERN.fullmatch(html_url)
        if (
            url_match is None
            or url_match.group(1) != repository.owner
            or url_match.group(2) != repository.name
            or int(url_match.group(3)) != run_id
        ):
            raise GitHubCIError(
                "Matching workflow run URL does not match its repository and id"
            )
        matches.append(
            WorkflowRun(
                run_id=run_id,
                status=status,
                conclusion=conclusion,
                html_url=html_url,
            )
        )
    return max(matches, key=lambda run: run.run_id, default=None)


def wait_for_workflow(
    fetch_runs: Callable[[], dict[str, Any]],
    repository: Repository,
    branch: str,
    sha: str,
    timeout_seconds: float,
    poll_seconds: float,
    *,
    monotonic: Callable[[], float] = time.monotonic,
    sleep: Callable[[float], None] = time.sleep,
) -> str:
    deadline = monotonic() + timeout_seconds
    while True:
        run = select_run(fetch_runs(), repository, branch, sha)
        if run is not None:
            if run.status == "completed":
                if run.conclusion != "success":
                    raise GitHubCIError(
                        f"GitHub Actions run {run.run_id} completed with "
                        f"{run.conclusion or 'no conclusion'}: {run.html_url}"
                    )
                return run.html_url
            if run.status not in PENDING_STATES:
                raise GitHubCIError(
                    f"GitHub Actions run {run.run_id} has unknown status "
                    f"{run.status}: {run.html_url}"
                )
        now = monotonic()
        if now >= deadline:
            raise GitHubCIError(
                f"Timed out waiting for GitHub Actions for {branch} at {sha}"
            )
        sleep(min(poll_seconds, deadline - now))


def wait_command(args: argparse.Namespace) -> None:
    root = args.root.resolve()
    if not SHA_PATTERN.fullmatch(args.sha):
        raise GitHubCIError("candidate SHA must be 40 lowercase hex characters")
    if not BRANCH_PATTERN.fullmatch(args.branch):
        raise GitHubCIError("candidate branch must match ci/XT-NNN")
    if not WORKFLOW_PATTERN.fullmatch(args.workflow):
        raise GitHubCIError("workflow must be a safe workflow filename")
    if args.timeout_seconds <= 0 or args.poll_seconds <= 0:
        raise GitHubCIError("timeout and poll interval must be positive")
    repository = load_repository(root, args.remote)
    username, password = load_credentials(root)
    client = GitHubClient(repository, username, password)
    result = wait_for_workflow(
        lambda: client.workflow_runs(args.workflow, args.branch),
        repository,
        args.branch,
        args.sha,
        args.timeout_seconds,
        args.poll_seconds,
    )
    print(result)


def main() -> int:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    wait_parser = subparsers.add_parser("wait")
    wait_parser.add_argument("--root", type=Path, required=True)
    wait_parser.add_argument("--remote", default="origin")
    wait_parser.add_argument("--workflow", default="ci.yml")
    wait_parser.add_argument("--branch", required=True)
    wait_parser.add_argument("--sha", required=True)
    wait_parser.add_argument("--timeout-seconds", type=float, default=5400)
    wait_parser.add_argument("--poll-seconds", type=float, default=15)
    args = parser.parse_args()
    try:
        if args.command == "wait":
            wait_command(args)
    except (GitHubCIError, OSError) as error:
        print(f"Remote CI error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
