#!/usr/bin/env python3
"""Collect normalized exact-candidate evidence from GitHub Actions."""

from __future__ import annotations

import io
import hashlib
import json
import re
import subprocess
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path, PurePosixPath
from typing import Any, Dict, List, Mapping, Optional, Sequence


MAX_RESPONSE_BYTES = 16 * 1024 * 1024
MAX_ARTIFACT_BYTES = 64 * 1024 * 1024
MAX_ARTIFACT_ENTRIES = 32


class GitHubEvidenceError(RuntimeError):
    """Raised when hosted workflow evidence is unavailable or malformed."""


class _CredentialSafeRedirectHandler(urllib.request.HTTPRedirectHandler):
    def redirect_request(
        self,
        request: urllib.request.Request,
        fp: Any,
        code: int,
        message: str,
        headers: Mapping[str, str],
        new_url: str,
    ) -> Optional[urllib.request.Request]:
        redirected = super().redirect_request(
            request,
            fp,
            code,
            message,
            headers,
            new_url,
        )
        if redirected is None:
            return None
        source = urllib.parse.urlsplit(request.full_url)
        target = urllib.parse.urlsplit(new_url)
        if (source.scheme, source.netloc) != (target.scheme, target.netloc):
            redirected.remove_header("Authorization")
        return redirected


def _reject_duplicate_pairs(pairs: Sequence[tuple[str, Any]]) -> Dict[str, Any]:
    result: Dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise GitHubEvidenceError("duplicate JSON key: {}".format(key))
        result[key] = value
    return result


def repository_slug(remote_url: str) -> str:
    patterns = (
        r"^https://github\.com/([^/]+/[^/]+?)(?:\.git)?$",
        r"^git@github\.com:([^/]+/[^/]+?)(?:\.git)?$",
        r"^ssh://git@github\.com/([^/]+/[^/]+?)(?:\.git)?$",
    )
    for pattern in patterns:
        match = re.fullmatch(pattern, remote_url)
        if match is not None:
            return match.group(1)
    raise GitHubEvidenceError("remote is not a supported GitHub repository URL")


def credential_token(root: Path) -> str:
    request = b"protocol=https\nhost=github.com\n\n"
    result = subprocess.run(
        ["git", "-C", str(root), "credential", "fill"],
        input=request,
        check=False,
        capture_output=True,
    )
    if result.returncode != 0:
        raise GitHubEvidenceError("Git credential helper did not provide credentials")
    values: Dict[str, str] = {}
    for raw_line in result.stdout.decode("utf-8", errors="strict").splitlines():
        if "=" not in raw_line:
            continue
        key, value = raw_line.split("=", 1)
        values[key] = value
    token = values.get("password", "")
    if not token:
        raise GitHubEvidenceError("GitHub credential has no token")
    return token


class GitHubClient:
    def __init__(self, repository: str, token: str) -> None:
        self.repository = repository
        self.token = token
        self.base = "https://api.github.com/repos/{}".format(repository)

    def _request(self, url: str, accept: str) -> bytes:
        request = urllib.request.Request(
            url,
            headers={
                "Accept": accept,
                "Authorization": "Bearer {}".format(self.token),
                "User-Agent": "xnn-transfer-harness-v2",
                "X-GitHub-Api-Version": "2022-11-28",
            },
        )
        try:
            opener = urllib.request.build_opener(_CredentialSafeRedirectHandler())
            with opener.open(request, timeout=30) as response:
                data = response.read(MAX_RESPONSE_BYTES + 1)
        except urllib.error.HTTPError as error:
            raise GitHubEvidenceError(
                "evidence request failed with HTTP {}".format(error.code)
            ) from error
        except (urllib.error.URLError, TimeoutError) as error:
            raise GitHubEvidenceError("evidence request failed") from error
        if len(data) > MAX_RESPONSE_BYTES:
            raise GitHubEvidenceError("GitHub API response exceeds size limit")
        return data

    def json(self, url: str) -> Mapping[str, Any]:
        data = self._request(url, "application/vnd.github+json")
        try:
            value = json.loads(
                data.decode("utf-8"),
                object_pairs_hook=_reject_duplicate_pairs,
            )
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise GitHubEvidenceError("GitHub API returned invalid JSON") from error
        if not isinstance(value, dict):
            raise GitHubEvidenceError("GitHub API response must be an object")
        return value

    def bytes(self, url: str) -> bytes:
        data = self._request(url, "application/vnd.github+json")
        if len(data) > MAX_ARTIFACT_BYTES:
            raise GitHubEvidenceError("artifact exceeds size limit")
        return data


def _complete_array(
    payload: Mapping[str, Any], key: str, count_key: str
) -> List[Mapping[str, Any]]:
    values = payload.get(key)
    total = payload.get(count_key)
    if not isinstance(values, list) or not isinstance(total, int):
        raise GitHubEvidenceError("{} response is malformed".format(key))
    if total != len(values):
        raise GitHubEvidenceError("{} response is paginated or incomplete".format(key))
    if not all(isinstance(item, dict) for item in values):
        raise GitHubEvidenceError("{} entries are malformed".format(key))
    return list(values)


def _artifact_json(archive: bytes) -> Mapping[str, Any]:
    try:
        with zipfile.ZipFile(io.BytesIO(archive)) as bundle:
            entries = bundle.infolist()
            if not entries or len(entries) > MAX_ARTIFACT_ENTRIES:
                raise GitHubEvidenceError("artifact entry count is invalid")
            total = 0
            names = set()
            for entry in entries:
                path = PurePosixPath(entry.filename)
                if path.is_absolute() or ".." in path.parts or entry.filename in names:
                    raise GitHubEvidenceError("artifact contains an unsafe path")
                names.add(entry.filename)
                total += entry.file_size
                if total > MAX_ARTIFACT_BYTES:
                    raise GitHubEvidenceError("artifact expansion exceeds limit")
            if "evidence.json" not in names:
                raise GitHubEvidenceError("artifact lacks evidence.json")
            raw = bundle.read("evidence.json")
    except (zipfile.BadZipFile, KeyError) as error:
        raise GitHubEvidenceError("artifact is not a valid evidence ZIP") from error
    try:
        manifest = json.loads(
            raw.decode("utf-8"),
            object_pairs_hook=_reject_duplicate_pairs,
        )
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise GitHubEvidenceError("artifact evidence is invalid JSON") from error
    if not isinstance(manifest, dict):
        raise GitHubEvidenceError("artifact evidence must be an object")
    return manifest


def _artifact_manifest(archive: bytes, expected_sha: str) -> Mapping[str, Any]:
    manifest = _artifact_json(archive)
    if not isinstance(manifest, dict) or set(manifest) != {
        "schema_version",
        "source_sha",
        "platform",
        "gate_ids",
        "gate_attestations",
        "criterion_ids",
        "criterion_evidence",
    }:
        raise GitHubEvidenceError("artifact evidence has invalid fields")
    if manifest["schema_version"] != 1 or manifest["source_sha"] != expected_sha:
        raise GitHubEvidenceError("artifact evidence has stale source SHA")
    if manifest["platform"] not in {"linux", "macos", "windows"}:
        raise GitHubEvidenceError("artifact platform is invalid")
    for field in ("gate_ids", "criterion_ids"):
        values = manifest[field]
        if (
            not isinstance(values, list)
            or not values
            or len(values) != len(set(values))
            or any(not isinstance(item, str) or not item for item in values)
        ):
            raise GitHubEvidenceError("artifact {} is invalid".format(field))
    for field in ("gate_attestations", "criterion_evidence"):
        values = manifest[field]
        if not isinstance(values, list) or any(
            not isinstance(item, str) or re.fullmatch(r"[0-9a-f]{64}", item) is None
            for item in values
        ):
            raise GitHubEvidenceError("artifact {} is invalid".format(field))
    if len(manifest["gate_ids"]) != len(manifest["gate_attestations"]):
        raise GitHubEvidenceError("artifact Gate identities are incomplete")
    if len(manifest["criterion_ids"]) != len(manifest["criterion_evidence"]):
        raise GitHubEvidenceError("artifact criterion identities are incomplete")
    return manifest


def _bootstrap_artifact_manifest(
    archive: bytes,
    expected_sha: str,
    expected_tree: str,
) -> Mapping[str, Any]:
    manifest = _artifact_json(archive)
    if set(manifest) != {
        "schema_version",
        "kind",
        "source_sha",
        "source_tree",
        "platform",
        "plan_sha256",
        "gate_ids",
        "gate_attestations",
        "skipped",
    }:
        raise GitHubEvidenceError("bootstrap artifact has invalid fields")
    if (
        manifest["schema_version"] != 1
        or manifest["kind"] != "bootstrap_cutover"
        or manifest["source_sha"] != expected_sha
        or manifest["source_tree"] != expected_tree
    ):
        raise GitHubEvidenceError("bootstrap artifact has stale source identity")
    if manifest["platform"] not in {"linux", "macos", "windows"}:
        raise GitHubEvidenceError("bootstrap artifact platform is invalid")
    if (
        not isinstance(manifest["plan_sha256"], str)
        or re.fullmatch(r"[0-9a-f]{64}", manifest["plan_sha256"]) is None
    ):
        raise GitHubEvidenceError("bootstrap artifact plan digest is invalid")
    gate_ids = manifest["gate_ids"]
    gate_attestations = manifest["gate_attestations"]
    if (
        not isinstance(gate_ids, list)
        or not gate_ids
        or len(gate_ids) != len(set(gate_ids))
        or any(
            not isinstance(gate_id, str)
            or re.fullmatch(r"[a-z][a-z0-9_]*", gate_id) is None
            for gate_id in gate_ids
        )
    ):
        raise GitHubEvidenceError("bootstrap artifact Gate IDs are invalid")
    if (
        not isinstance(gate_attestations, list)
        or len(gate_attestations) != len(gate_ids)
        or len(gate_attestations) != len(set(gate_attestations))
        or any(
            not isinstance(digest, str) or re.fullmatch(r"[0-9a-f]{64}", digest) is None
            for digest in gate_attestations
        )
    ):
        raise GitHubEvidenceError("bootstrap artifact Gate evidence is invalid")
    if manifest["skipped"] is not False:
        raise GitHubEvidenceError("bootstrap artifact reports skipped execution")
    return manifest


def _collect_run(
    client: GitHubClient,
    *,
    workflow_path: str,
    branch: str,
    candidate_sha: str,
) -> tuple[int, int, List[Dict[str, str]], Dict[str, Mapping[str, Any]]]:
    workflow_name = urllib.parse.quote(workflow_path, safe="")
    query = urllib.parse.urlencode({"branch": branch, "event": "push", "per_page": 100})
    runs_payload = client.json(
        "{}/actions/workflows/{}/runs?{}".format(client.base, workflow_name, query)
    )
    runs = _complete_array(runs_payload, "workflow_runs", "total_count")
    candidates = [
        run
        for run in runs
        if run.get("head_sha") == candidate_sha
        and run.get("head_branch") == branch
        and run.get("event") == "push"
        and run.get("path") == workflow_path
        and run.get("status") == "completed"
    ]
    if not candidates:
        raise GitHubEvidenceError("no completed exact-candidate workflow run")
    candidates.sort(
        key=lambda run: (
            int(run.get("run_attempt", 0)),
            int(run.get("id", 0)),
        ),
        reverse=True,
    )
    run = candidates[0]
    if run.get("conclusion") != "success":
        raise GitHubEvidenceError("latest exact-candidate workflow did not succeed")
    run_id = run.get("id")
    run_attempt = run.get("run_attempt")
    if (
        not isinstance(run_id, int)
        or run_id < 1
        or not isinstance(run_attempt, int)
        or run_attempt < 1
    ):
        raise GitHubEvidenceError("workflow run identity is invalid")
    jobs_payload = client.json(
        "{}/actions/runs/{}/jobs?per_page=100".format(client.base, run_id)
    )
    jobs = _complete_array(jobs_payload, "jobs", "total_count")
    normalized_jobs = []
    for job in jobs:
        name = job.get("name")
        conclusion = job.get("conclusion")
        if not isinstance(name, str) or not isinstance(conclusion, str):
            raise GitHubEvidenceError("workflow job is malformed")
        normalized_jobs.append({"name": name, "conclusion": conclusion})
    artifacts_payload = client.json(
        "{}/actions/runs/{}/artifacts?per_page=100".format(client.base, run_id)
    )
    artifacts = _complete_array(artifacts_payload, "artifacts", "total_count")
    by_name: Dict[str, Mapping[str, Any]] = {}
    for artifact in artifacts:
        name = artifact.get("name")
        if not isinstance(name, str) or name in by_name:
            raise GitHubEvidenceError("artifact names are invalid")
        by_name[name] = artifact
    return run_id, run_attempt, normalized_jobs, by_name


def _download_artifact(
    client: GitHubClient,
    artifacts: Mapping[str, Mapping[str, Any]],
    name: str,
) -> tuple[bytes, int]:
    if name not in artifacts:
        raise GitHubEvidenceError("required artifact {} is missing".format(name))
    artifact = artifacts[name]
    if artifact.get("expired") is not False:
        raise GitHubEvidenceError("required artifact {} is expired".format(name))
    artifact_id = artifact.get("id")
    if not isinstance(artifact_id, int) or artifact_id < 1:
        raise GitHubEvidenceError("required artifact id is invalid")
    archive = client.bytes(
        "{}/actions/artifacts/{}/zip".format(client.base, artifact_id)
    )
    return archive, artifact_id


def collect_workflow_evidence(
    client: GitHubClient,
    *,
    workflow_path: str,
    workflow_blob: str,
    branch: str,
    candidate_sha: str,
    required_artifacts: Sequence[str],
) -> Dict[str, Any]:
    run_id, run_attempt, normalized_jobs, by_name = _collect_run(
        client,
        workflow_path=workflow_path,
        branch=branch,
        candidate_sha=candidate_sha,
    )
    normalized_artifacts = []
    for name in required_artifacts:
        archive, _ = _download_artifact(client, by_name, name)
        manifest = _artifact_manifest(archive, candidate_sha)
        normalized_artifacts.append(
            {
                "name": name,
                "source_sha": manifest["source_sha"],
                "sha256": hashlib_sha256(archive),
                "platform": manifest["platform"],
                "gate_ids": manifest["gate_ids"],
                "gate_attestations": manifest["gate_attestations"],
                "criterion_ids": manifest["criterion_ids"],
                "criterion_evidence": manifest["criterion_evidence"],
            }
        )
    return {
        "repository": client.repository,
        "workflow_path": workflow_path,
        "workflow_blob": workflow_blob,
        "run_id": run_id,
        "run_attempt": run_attempt,
        "head_sha": candidate_sha,
        "head_branch": branch,
        "event": "push",
        "conclusion": "success",
        "jobs": normalized_jobs,
        "artifacts": normalized_artifacts,
    }


def collect_bootstrap_workflow_evidence(
    client: GitHubClient,
    *,
    workflow_path: str,
    workflow_blob: str,
    branch: str,
    candidate_sha: str,
    candidate_tree: str,
    required_artifacts: Sequence[str],
) -> Dict[str, Any]:
    run_id, run_attempt, normalized_jobs, by_name = _collect_run(
        client,
        workflow_path=workflow_path,
        branch=branch,
        candidate_sha=candidate_sha,
    )
    normalized_artifacts = []
    for name in required_artifacts:
        archive, artifact_id = _download_artifact(client, by_name, name)
        manifest = _bootstrap_artifact_manifest(
            archive,
            candidate_sha,
            candidate_tree,
        )
        normalized_artifacts.append(
            {
                "name": name,
                "artifact_id": artifact_id,
                "source_sha": manifest["source_sha"],
                "source_tree": manifest["source_tree"],
                "sha256": hashlib_sha256(archive),
                "platform": manifest["platform"],
                "plan_sha256": manifest["plan_sha256"],
                "gate_ids": manifest["gate_ids"],
                "gate_attestations": manifest["gate_attestations"],
                "skipped": manifest["skipped"],
            }
        )
    return {
        "repository": client.repository,
        "workflow_path": workflow_path,
        "workflow_blob": workflow_blob,
        "run_id": run_id,
        "run_attempt": run_attempt,
        "head_sha": candidate_sha,
        "head_branch": branch,
        "event": "push",
        "conclusion": "success",
        "jobs": normalized_jobs,
        "artifacts": normalized_artifacts,
    }


def hashlib_sha256(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()
