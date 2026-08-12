#!/usr/bin/env python3
"""Tests for one-time Harness V2 bootstrap acceptance."""

from __future__ import annotations

import sys
import subprocess
import unittest
from pathlib import Path

HARNESS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HARNESS))

import bootstrap  # noqa: E402
import git_ops  # noqa: E402
from test_gates_tdd import GateRepository  # noqa: E402


WORKFLOW = ".github/workflows/merge-queue.yml"
WORKFLOW_BLOB = "b" * 40
PLAN_SHA = "c" * 64
JOBS = ["Candidate plan", "Harness V2", "Candidate accepted"]
ARTIFACT_NAMES = [
    "candidate-evidence-linux",
    "candidate-evidence-macos",
    "candidate-evidence-windows",
]
EXPECTED_GATES = {
    "linux": ["feature_test"],
    "macos": ["feature_test"],
    "windows": ["feature_test"],
}
OWNER = {
    "kind": "project-owner",
    "id": "project-owner",
    "name": "Project Owner",
    "email": "owner@example.com",
}


def workflow_evidence(candidate_sha: str, candidate_tree: str) -> dict:
    artifacts = []
    for index, platform in enumerate(("linux", "macos", "windows"), start=1):
        artifacts.append(
            {
                "name": "candidate-evidence-{}".format(platform),
                "artifact_id": index,
                "source_sha": candidate_sha,
                "source_tree": candidate_tree,
                "sha256": "{:064x}".format(index),
                "platform": platform,
                "plan_sha256": PLAN_SHA,
                "gate_ids": ["feature_test"],
                "gate_attestations": ["{:064x}".format(index + 10)],
                "skipped": False,
            }
        )
    return {
        "repository": "example/XnnTransfer",
        "workflow_path": WORKFLOW,
        "workflow_blob": WORKFLOW_BLOB,
        "run_id": 42,
        "run_attempt": 1,
        "head_sha": candidate_sha,
        "head_branch": "queue/bootstrap/cutover",
        "event": "push",
        "conclusion": "success",
        "jobs": [{"name": name, "conclusion": "success"} for name in JOBS],
        "artifacts": artifacts,
    }


class BootstrapAcceptanceTest(unittest.TestCase):
    def test_validates_complete_matrix_and_writes_immutable_ref(self) -> None:
        repository = GateRepository(self)
        candidate_sha = repository.commit("chore: initialize")
        candidate_tree = git_ops.current_tree(repository.root, candidate_sha)
        evidence = workflow_evidence(candidate_sha, candidate_tree)
        normalized = bootstrap.validate_workflow_evidence(
            evidence,
            repository="example/XnnTransfer",
            workflow_path=WORKFLOW,
            workflow_blob=WORKFLOW_BLOB,
            candidate_sha=candidate_sha,
            candidate_tree=candidate_tree,
            candidate_branch="queue/bootstrap/cutover",
            required_jobs=JOBS,
            required_artifacts=ARTIFACT_NAMES,
            expected_plan_sha256=PLAN_SHA,
            expected_gates=EXPECTED_GATES,
        )
        value = bootstrap.create_attestation(
            candidate_sha=candidate_sha,
            candidate_tree=candidate_tree,
            integration_base=candidate_sha,
            workflow=normalized,
            required_jobs=JOBS,
            required_artifacts=ARTIFACT_NAMES,
            actor=OWNER,
            created_at="2026-08-12T00:00:00Z",
        )
        contracts = repository.load()
        store = bootstrap.AcceptanceStore(contracts, None)
        commit = store.write(value)
        self.assertEqual(
            git_ops.ref_sha(repository.root, store.ref(candidate_sha)),
            commit,
        )
        self.assertEqual(
            git_ops.read_json_object(repository.root, commit, "attestation.json"),
            value,
        )
        changed = dict(value)
        changed["created_at"] = "2026-08-12T00:00:01Z"
        with self.assertRaisesRegex(bootstrap.BootstrapError, "other evidence"):
            store.write(changed)
        wrong_actor = dict(value)
        wrong_actor["created_by"] = {**OWNER, "id": "other"}
        with self.assertRaisesRegex(bootstrap.BootstrapError, "project owner"):
            store.write(wrong_actor)

    def test_rejects_skipped_job_and_mismatched_plan(self) -> None:
        repository = GateRepository(self)
        candidate_sha = repository.commit("chore: initialize")
        candidate_tree = git_ops.current_tree(repository.root, candidate_sha)
        evidence = workflow_evidence(candidate_sha, candidate_tree)
        evidence["jobs"][0]["conclusion"] = "skipped"
        with self.assertRaisesRegex(bootstrap.BootstrapError, "skipped jobs"):
            bootstrap.validate_workflow_evidence(
                evidence,
                repository="example/XnnTransfer",
                workflow_path=WORKFLOW,
                workflow_blob=WORKFLOW_BLOB,
                candidate_sha=candidate_sha,
                candidate_tree=candidate_tree,
                candidate_branch="queue/bootstrap/cutover",
                required_jobs=JOBS,
                required_artifacts=ARTIFACT_NAMES,
                expected_plan_sha256=PLAN_SHA,
                expected_gates=EXPECTED_GATES,
            )

        evidence = workflow_evidence(candidate_sha, candidate_tree)
        evidence["artifacts"][0]["plan_sha256"] = "d" * 64
        with self.assertRaisesRegex(bootstrap.BootstrapError, "binding"):
            bootstrap.validate_workflow_evidence(
                evidence,
                repository="example/XnnTransfer",
                workflow_path=WORKFLOW,
                workflow_blob=WORKFLOW_BLOB,
                candidate_sha=candidate_sha,
                candidate_tree=candidate_tree,
                candidate_branch="queue/bootstrap/cutover",
                required_jobs=JOBS,
                required_artifacts=ARTIFACT_NAMES,
                expected_plan_sha256=PLAN_SHA,
                expected_gates=EXPECTED_GATES,
            )

    def test_protected_publication_uses_cas_and_is_recoverable(self) -> None:
        repository = GateRepository(self)
        base = repository.commit("chore: initialize")
        candidate_sha = repository.commit(
            "fix(harness): candidate",
            allow_empty=True,
        )
        candidate_tree = git_ops.current_tree(repository.root, candidate_sha)
        evidence = workflow_evidence(candidate_sha, candidate_tree)
        value = bootstrap.create_attestation(
            candidate_sha=candidate_sha,
            candidate_tree=candidate_tree,
            integration_base=base,
            workflow=evidence,
            required_jobs=JOBS,
            required_artifacts=ARTIFACT_NAMES,
            actor=OWNER,
            created_at="2026-08-12T00:00:00Z",
        )
        remote = repository.external / "remote.git"
        subprocess.run(
            ["git", "init", "--bare", "-q", str(remote)],
            check=True,
        )
        repository.git("remote", "add", "origin", str(remote))
        repository.git("push", "-q", "origin", "{}:refs/heads/harness".format(base))
        contracts = repository.load()

        self.assertEqual(
            bootstrap.publish_candidate(contracts, "origin", value),
            "published",
        )
        self.assertEqual(
            git_ops.remote_ref_sha(
                repository.root,
                "origin",
                "refs/heads/harness",
            ),
            candidate_sha,
        )
        self.assertEqual(
            bootstrap.publish_candidate(contracts, "origin", value),
            "already_published",
        )


if __name__ == "__main__":
    unittest.main()
