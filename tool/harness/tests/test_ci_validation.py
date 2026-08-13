#!/usr/bin/env python3
"""Tests for one-time live CI result validation."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path


HARNESS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HARNESS))

import ci_validation  # noqa: E402


class CIValidationTest(unittest.TestCase):
    def _result(self) -> dict:
        return {
            "repository": "example/repository",
            "workflow_path": ".github/workflows/merge-queue.yml",
            "workflow_blob": "b" * 40,
            "run_id": 1,
            "run_attempt": 1,
            "head_sha": "a" * 40,
            "head_branch": "queue/train/001-XT-101",
            "event": "push",
            "conclusion": "success",
            "jobs": [
                {"name": "Harness V2", "conclusion": "success"},
                {"name": "Candidate accepted", "conclusion": "success"},
            ],
            "artifacts": [
                {
                    "name": "candidate-evidence-linux",
                    "source_sha": "a" * 40,
                    "sha256": "c" * 64,
                    "platform": "linux",
                    "gate_ids": ["feature_test"],
                    "gate_attestations": ["d" * 64],
                    "criterion_ids": ["CRIT-EXAMPLE"],
                    "criterion_evidence": ["e" * 64],
                }
            ],
        }

    def _validate(self, value: dict) -> dict:
        return ci_validation.validate_workflow_result(
            value,
            repository="example/repository",
            workflow_path=".github/workflows/merge-queue.yml",
            workflow_blob="b" * 40,
            candidate_sha="a" * 40,
            candidate_branch="queue/train/001-XT-101",
            required_jobs=["Harness V2", "Candidate accepted"],
            required_artifacts=["candidate-evidence-linux"],
        )

    def test_accepts_exact_successful_result(self) -> None:
        self.assertEqual(self._validate(self._result())["run_id"], 1)

    def test_rejects_skipped_jobs_and_stale_artifacts(self) -> None:
        skipped = self._result()
        skipped["jobs"][0]["conclusion"] = "skipped"
        with self.assertRaisesRegex(
            ci_validation.CIValidationError,
            "skipped jobs",
        ):
            self._validate(skipped)

        stale = self._result()
        stale["artifacts"][0]["source_sha"] = "f" * 40
        with self.assertRaisesRegex(
            ci_validation.CIValidationError,
            "stale source SHA",
        ):
            self._validate(stale)

    def test_rejects_missing_required_job_and_artifact(self) -> None:
        missing_job = self._result()
        missing_job["jobs"] = missing_job["jobs"][:1]
        with self.assertRaisesRegex(
            ci_validation.CIValidationError,
            "did not succeed",
        ):
            self._validate(missing_job)

        missing_artifact = self._result()
        missing_artifact["artifacts"] = []
        with self.assertRaisesRegex(
            ci_validation.CIValidationError,
            "required artifacts are missing",
        ):
            self._validate(missing_artifact)


if __name__ == "__main__":
    unittest.main()
