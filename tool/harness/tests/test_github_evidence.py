#!/usr/bin/env python3
"""Tests for exact GitHub workflow evidence collection."""

from __future__ import annotations

import io
import json
import sys
import unittest
import zipfile
from pathlib import Path
from typing import Any, Dict, Mapping

HARNESS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HARNESS))

import github_evidence  # noqa: E402


SHA = "a" * 40
WORKFLOW_BLOB = "b" * 40


def artifact_zip(source_sha: str = SHA, unsafe: bool = False) -> bytes:
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w") as bundle:
        bundle.writestr(
            "evidence.json",
            json.dumps(
                {
                    "schema_version": 1,
                    "source_sha": source_sha,
                    "gate_attestations": ["c" * 64],
                    "criterion_evidence": ["d" * 64],
                }
            ),
        )
        if unsafe:
            bundle.writestr("../escape.txt", "bad")
    return output.getvalue()


class FakeClient:
    def __init__(self) -> None:
        self.repository = "example/XnnTransfer"
        self.base = "https://api.github.test/repos/example/XnnTransfer"
        self.archive = artifact_zip()
        self.runs: Dict[str, Any] = {
            "total_count": 1,
            "workflow_runs": [
                {
                    "id": 42,
                    "run_attempt": 1,
                    "head_sha": SHA,
                    "head_branch": "queue/train/001-XT-101",
                    "event": "push",
                    "path": ".github/workflows/merge-queue.yml",
                    "status": "completed",
                    "conclusion": "success",
                }
            ],
        }
        self.jobs: Dict[str, Any] = {
            "total_count": 2,
            "jobs": [
                {"name": "Harness V2", "conclusion": "success"},
                {"name": "Product gates", "conclusion": "success"},
            ],
        }
        self.artifacts: Dict[str, Any] = {
            "total_count": 1,
            "artifacts": [
                {
                    "id": 7,
                    "name": "candidate-evidence",
                    "expired": False,
                }
            ],
        }

    def json(self, url: str) -> Mapping[str, Any]:
        if "/jobs?" in url:
            return self.jobs
        if "/artifacts?" in url:
            return self.artifacts
        if "/runs?" in url:
            return self.runs
        raise AssertionError("unexpected URL {}".format(url))

    def bytes(self, url: str) -> bytes:
        if not url.endswith("/actions/artifacts/7/zip"):
            raise AssertionError("unexpected artifact URL {}".format(url))
        return self.archive


class GitHubEvidenceTest(unittest.TestCase):
    def collect(self, client: FakeClient) -> Mapping[str, Any]:
        return github_evidence.collect_workflow_evidence(
            client,
            workflow_path=".github/workflows/merge-queue.yml",
            workflow_blob=WORKFLOW_BLOB,
            branch="queue/train/001-XT-101",
            candidate_sha=SHA,
            required_artifacts=["candidate-evidence"],
        )

    def test_parses_supported_repository_urls(self) -> None:
        self.assertEqual(
            github_evidence.repository_slug(
                "https://github.com/example/XnnTransfer.git"
            ),
            "example/XnnTransfer",
        )
        self.assertEqual(
            github_evidence.repository_slug(
                "git@github.com:example/XnnTransfer.git"
            ),
            "example/XnnTransfer",
        )
        with self.assertRaises(github_evidence.GitHubEvidenceError):
            github_evidence.repository_slug("https://example.com/repo.git")

    def test_collects_exact_run_jobs_and_downloaded_artifact(self) -> None:
        evidence = self.collect(FakeClient())
        self.assertEqual(evidence["run_id"], 42)
        self.assertEqual(evidence["head_sha"], SHA)
        self.assertEqual(len(evidence["jobs"]), 2)
        self.assertEqual(
            evidence["artifacts"][0]["source_sha"], SHA
        )
        self.assertEqual(len(evidence["artifacts"][0]["sha256"]), 64)

    def test_latest_attempt_must_succeed(self) -> None:
        client = FakeClient()
        previous = dict(client.runs["workflow_runs"][0])
        previous["id"] = 41
        previous["run_attempt"] = 1
        latest = dict(previous)
        latest["id"] = 42
        latest["run_attempt"] = 2
        latest["conclusion"] = "failure"
        client.runs = {
            "total_count": 2,
            "workflow_runs": [previous, latest],
        }
        with self.assertRaisesRegex(
            github_evidence.GitHubEvidenceError, "did not succeed"
        ):
            self.collect(client)

    def test_rejects_incomplete_api_page(self) -> None:
        client = FakeClient()
        client.jobs["total_count"] = 101
        with self.assertRaisesRegex(
            github_evidence.GitHubEvidenceError, "incomplete"
        ):
            self.collect(client)

    def test_rejects_stale_or_unsafe_artifact(self) -> None:
        client = FakeClient()
        client.archive = artifact_zip("f" * 40)
        with self.assertRaisesRegex(
            github_evidence.GitHubEvidenceError, "stale source"
        ):
            self.collect(client)

        client = FakeClient()
        client.archive = artifact_zip(unsafe=True)
        with self.assertRaisesRegex(
            github_evidence.GitHubEvidenceError, "unsafe path"
        ):
            self.collect(client)


if __name__ == "__main__":
    unittest.main()
