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

import evidence as criterion_evidence


SHA_PATTERN = re.compile(r"^[0-9a-f]{40}$")
BRANCH_PATTERN = re.compile(r"^ci/XT-[0-9]{3,}$")
TASK_PATTERN = re.compile(r"^XT-[0-9]{3,}$")
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
MAX_JSON_BYTES = 4 * 1024 * 1024
MAX_ARTIFACT_BYTES = criterion_evidence.MAX_ARTIFACT_BYTES


class GitHubCIError(RuntimeError):
    pass


@dataclass(frozen=True)
class Repository:
    owner: str
    name: str


@dataclass(frozen=True)
class WorkflowRun:
    run_id: int
    run_attempt: int
    status: str
    conclusion: str | None
    html_url: str
    event: str
    head_branch: str
    head_sha: str


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

    def _url(self, suffix: str) -> str:
        owner = urllib.parse.quote(self.repository.owner, safe="")
        name = urllib.parse.quote(self.repository.name, safe="")
        return f"https://api.github.com/repos/{owner}/{name}/{suffix}"

    def _request_bytes(
        self,
        url: str,
        label: str,
        limit: int,
    ) -> bytes:
        request = urllib.request.Request(url, headers=self.headers)
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                raw_length = response.headers.get("Content-Length")
                if raw_length is not None:
                    try:
                        content_length = int(raw_length)
                    except ValueError as error:
                        raise GitHubCIError(
                            f"GitHub Actions {label} response has invalid size"
                        ) from error
                    if content_length < 0 or content_length > limit:
                        raise GitHubCIError(
                            f"GitHub Actions {label} exceeds the size limit"
                        )
                content = response.read(limit + 1)
        except urllib.error.HTTPError as error:
            raise GitHubCIError(
                f"GitHub Actions {label} request returned HTTP {error.code}"
            ) from error
        except GitHubCIError:
            raise
        except (urllib.error.URLError, TimeoutError, OSError) as error:
            raise GitHubCIError(
                f"GitHub Actions {label} request failed"
            ) from error
        if len(content) > limit:
            raise GitHubCIError(
                f"GitHub Actions {label} exceeds the size limit"
            )
        return content

    def _request_json(self, url: str, label: str) -> dict[str, Any]:
        content = self._request_bytes(url, label, MAX_JSON_BYTES)
        try:
            payload = json.loads(content)
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise GitHubCIError(
                f"GitHub Actions {label} response is not valid JSON"
            ) from error
        if not isinstance(payload, dict):
            raise GitHubCIError(
                f"GitHub Actions {label} response is not an object"
            )
        return payload

    def workflow_runs(self, workflow: str, branch: str) -> dict[str, Any]:
        query = urllib.parse.urlencode(
            {"branch": branch, "event": "push", "per_page": "100"}
        )
        workflow_id = urllib.parse.quote(workflow, safe="")
        return self._request_json(
            self._url(f"actions/workflows/{workflow_id}/runs?{query}"),
            "workflow runs",
        )

    def workflow_jobs(self, run_id: int) -> dict[str, Any]:
        return self._request_json(
            self._url(f"actions/runs/{run_id}/jobs?per_page=100"),
            "jobs",
        )

    def workflow_artifacts(self, run_id: int) -> dict[str, Any]:
        return self._request_json(
            self._url(f"actions/runs/{run_id}/artifacts?per_page=100"),
            "artifacts",
        )

    def artifact_archive(self, artifact_id: int) -> bytes:
        return self._request_bytes(
            self._url(f"actions/artifacts/{artifact_id}/zip"),
            "artifact archive",
            MAX_ARTIFACT_BYTES,
        )


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
        run_attempt = raw.get("run_attempt")
        status = raw.get("status")
        conclusion = raw.get("conclusion")
        html_url = raw.get("html_url")
        if (
            not isinstance(run_id, int)
            or not isinstance(run_attempt, int)
            or run_attempt <= 0
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
                run_attempt=run_attempt,
                status=status,
                conclusion=conclusion,
                html_url=html_url,
                event=str(raw["event"]),
                head_branch=str(raw["head_branch"]),
                head_sha=str(raw["head_sha"]),
            )
        )
    return max(matches, key=lambda run: run.run_id, default=None)


def wait_for_workflow_run(
    fetch_runs: Callable[[], dict[str, Any]],
    repository: Repository,
    branch: str,
    sha: str,
    timeout_seconds: float,
    poll_seconds: float,
    *,
    monotonic: Callable[[], float] = time.monotonic,
    sleep: Callable[[float], None] = time.sleep,
) -> WorkflowRun:
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
                return run
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
    return wait_for_workflow_run(
        fetch_runs,
        repository,
        branch,
        sha,
        timeout_seconds,
        poll_seconds,
        monotonic=monotonic,
        sleep=sleep,
    ).html_url


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
    if (args.task_id is None) != (args.evidence_output is None):
        raise GitHubCIError(
            "task-id and evidence-output must be supplied together"
        )
    if args.task_id is not None:
        if not TASK_PATTERN.fullmatch(args.task_id):
            raise GitHubCIError("task-id must match XT-NNN")
        if args.branch != f"ci/{args.task_id}":
            raise GitHubCIError("candidate branch does not match task-id")
    repository = load_repository(root, args.remote)
    username, password = load_credentials(root)
    client = GitHubClient(repository, username, password)
    run = wait_for_workflow_run(
        lambda: client.workflow_runs(args.workflow, args.branch),
        repository,
        args.branch,
        args.sha,
        args.timeout_seconds,
        args.poll_seconds,
    )
    if args.evidence_output is not None and args.task_id is not None:
        bundle = criterion_evidence.collect_for_task(
            root,
            args.task_id,
            args.sha,
            {
                "id": run.run_id,
                "run_attempt": run.run_attempt,
                "status": run.status,
                "conclusion": run.conclusion,
                "html_url": run.html_url,
                "event": run.event,
                "head_branch": run.head_branch,
                "head_sha": run.head_sha,
            },
            client.workflow_jobs(run.run_id),
            client.workflow_artifacts(run.run_id),
            client.artifact_archive,
        )
        criterion_evidence.write_bundle(args.evidence_output, bundle)
    print(run.html_url)


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
    wait_parser.add_argument("--task-id")
    wait_parser.add_argument("--evidence-output", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "wait":
            wait_command(args)
    except (GitHubCIError, criterion_evidence.EvidenceError, OSError) as error:
        print(f"Remote CI error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
