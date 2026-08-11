#!/usr/bin/env python3

from __future__ import annotations

import copy
import hashlib
import io
import json
import tempfile
import unittest
import zipfile
from pathlib import Path

import evidence


SOURCE_SHA = "a" * 40
PLAN_SHA = "b" * 64
RUN_URL = "https://github.com/example/XnnTransfer/actions/runs/700"
WORKFLOW = b"name: CI\n"


def sha256(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


class CriterionEvidenceTest(unittest.TestCase):
    def setUp(self) -> None:
        self.task = {
            "id": "XT-083",
            "delivery_plan": "DP-FIXTURE",
            "requirement_ids": ["REQ-FIXTURE"],
            "delivery_role": "implementation",
        }
        self.record = {
            "schema_version": 4,
            "id": "XT-083",
            "state": "integrated",
            "integration": {"verified_sha": SOURCE_SHA},
            "verification": {
                "gates": ["focused", "verify"],
                "commands": ["make focused", "make verify"],
            },
            "test_contract": {
                "plan_content_sha256": PLAN_SHA,
                "criterion_ids": ["CRIT-FIXTURE-EXACT-BYTES"],
            },
        }
        self.plan = {
            "schema_version": 2,
            "id": "DP-FIXTURE",
            "status": "approved",
            "approval": {"content_sha256": PLAN_SHA},
            "requirements": [
                {
                    "id": "REQ-FIXTURE",
                    "acceptance_task": "XT-084",
                    "criteria": [
                        {
                            "id": "CRIT-FIXTURE-EXACT-BYTES",
                            "implementation_tasks": ["XT-083"],
                            "evidence": [
                                {
                                    "id": "EVD-FIXTURE-PACKAGED",
                                    "producer_task": "XT-083",
                                    "gate": "focused",
                                    "level": "e2e",
                                    "required_scenarios": [
                                        "transfer.explicit_accept"
                                    ],
                                    "required_assertions": [
                                        "destination.exact_bytes"
                                    ],
                                    "required_platforms": ["linux", "macos"],
                                    "required_roles": ["sender", "receiver"],
                                    "topology": "packaged_e2e",
                                    "allow_skipped": False,
                                }
                            ],
                        }
                    ],
                }
            ],
        }
        self.gates = {"focused": "make focused", "verify": "make verify"}
        self.run = {
            "id": 700,
            "run_attempt": 1,
            "event": "push",
            "head_branch": "ci/XT-083",
            "head_sha": SOURCE_SHA,
            "status": "completed",
            "conclusion": "success",
            "html_url": RUN_URL,
        }
        self.jobs: list[dict[str, object]] = []
        self.artifacts: list[dict[str, object]] = []
        self.archives: dict[int, bytes] = {}
        artifact_id = 900
        job_id = 800
        for platform in ("linux", "macos"):
            for role in ("sender", "receiver"):
                job_name = f"Packaged E2E ({platform}, {role})"
                self.jobs.append(
                    {
                        "id": job_id,
                        "run_attempt": 1,
                        "name": job_name,
                        "head_sha": SOURCE_SHA,
                        "status": "completed",
                        "conclusion": "success",
                        "html_url": f"{RUN_URL}/job/{job_id}",
                        "steps": [
                            {
                                "name": "Evidence gate: focused",
                                "status": "completed",
                                "conclusion": "success",
                            }
                        ],
                    }
                )
                binary_path = f"bin/{platform}-{role}/xnn-transfer"
                binary = f"{platform}:{role}:binary".encode("ascii")
                manifest = {
                    "schema_version": 1,
                    "criterion_id": "CRIT-FIXTURE-EXACT-BYTES",
                    "evidence_id": "EVD-FIXTURE-PACKAGED",
                    "source_sha": SOURCE_SHA,
                    "run_attempt": 1,
                    "gate": "focused",
                    "scenarios": ["transfer.explicit_accept"],
                    "assertions": ["destination.exact_bytes"],
                    "topology": "packaged_e2e",
                    "platform": platform,
                    "role": role,
                    "job_name": job_name,
                    "result": "passed",
                    "skipped": False,
                    "runtime": {
                        "process_count": 2,
                        "transport": "tcp_tls",
                        "authenticated": True,
                        "packaged": True,
                    },
                    "binaries": [
                        {
                            "path": binary_path,
                            "sha256": sha256(binary),
                        }
                    ],
                }
                archive = self.archive(manifest, {binary_path: binary})
                self.archives[artifact_id] = archive
                self.artifacts.append(
                    {
                        "id": artifact_id,
                        "name": (
                            "criterion-EVD-FIXTURE-PACKAGED-"
                            f"{platform}-{role}"
                        ),
                        "size_in_bytes": len(archive),
                        "expired": False,
                        "digest": f"sha256:{sha256(archive)}",
                        "archive_download_url": (
                            "https://api.github.com/repos/example/"
                            f"XnnTransfer/actions/artifacts/{artifact_id}/zip"
                        ),
                        "workflow_run": {
                            "id": 700,
                            "head_sha": SOURCE_SHA,
                        },
                    }
                )
                artifact_id += 1
                job_id += 1

    @staticmethod
    def archive(
        manifest: dict[str, object],
        files: dict[str, bytes],
    ) -> bytes:
        output = io.BytesIO()
        with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
            archive.writestr(
                "evidence.json",
                json.dumps(manifest, sort_keys=True),
            )
            for path, content in files.items():
                archive.writestr(path, content)
        return output.getvalue()

    def collect(self) -> dict[str, object]:
        return evidence.collect_bundle(
            task=self.task,
            record=self.record,
            plan=self.plan,
            source_sha=SOURCE_SHA,
            workflow_bytes=WORKFLOW,
            gate_registry=self.gates,
            run=self.run,
            jobs_payload={
                "total_count": len(self.jobs),
                "jobs": self.jobs,
            },
            artifacts_payload={
                "total_count": len(self.artifacts),
                "artifacts": self.artifacts,
            },
            download_artifact=self.archives.__getitem__,
        )

    def test_collects_exact_candidate_matrix_and_digests(self) -> None:
        bundle = self.collect()
        self.assertEqual(
            evidence.validate_bundle(
                self.task,
                self.record,
                self.plan,
                bundle,
                self.gates,
                SOURCE_SHA,
            ),
            [],
        )
        self.assertEqual(bundle["source_sha"], SOURCE_SHA)
        self.assertEqual(bundle["workflow_sha256"], sha256(WORKFLOW))
        criterion = bundle["criteria"][0]
        proof = criterion["evidence"][0]
        self.assertEqual(proof["result"], "passed")
        self.assertEqual(len(proof["matrix"]), 4)
        self.assertEqual(
            {
                (entry["platform"], entry["role"])
                for entry in proof["matrix"]
            },
            {
                ("linux", "sender"),
                ("linux", "receiver"),
                ("macos", "sender"),
                ("macos", "receiver"),
            },
        )
        self.assertTrue(
            all(entry["binary_digests"] for entry in proof["matrix"])
        )

    def test_rejects_stale_run_and_artifact_source(self) -> None:
        self.run["head_sha"] = "c" * 40
        with self.assertRaisesRegex(evidence.EvidenceError, "source SHA"):
            self.collect()
        self.run["head_sha"] = SOURCE_SHA
        self.artifacts[0]["workflow_run"]["head_sha"] = "c" * 40
        with self.assertRaisesRegex(evidence.EvidenceError, "source SHA"):
            self.collect()
        self.artifacts[0]["workflow_run"]["head_sha"] = SOURCE_SHA
        self.run["run_attempt"] = 2
        with self.assertRaisesRegex(evidence.EvidenceError, "run attempt"):
            self.collect()

    def test_rejects_missing_skipped_or_partial_job_matrix(self) -> None:
        self.jobs[0]["conclusion"] = "skipped"
        with self.assertRaisesRegex(evidence.EvidenceError, "successful job"):
            self.collect()
        self.jobs[0]["conclusion"] = "success"
        self.jobs[0]["steps"][0]["conclusion"] = "skipped"
        with self.assertRaisesRegex(evidence.EvidenceError, "gate step"):
            self.collect()
        self.jobs[0]["steps"][0]["conclusion"] = "success"
        removed = self.artifacts.pop()
        self.archives.pop(removed["id"])
        with self.assertRaisesRegex(evidence.EvidenceError, "matrix"):
            self.collect()

    def test_rejects_truncated_api_pages(self) -> None:
        with self.assertRaisesRegex(evidence.EvidenceError, "job response"):
            evidence.collect_bundle(
                task=self.task,
                record=self.record,
                plan=self.plan,
                source_sha=SOURCE_SHA,
                workflow_bytes=WORKFLOW,
                gate_registry=self.gates,
                run=self.run,
                jobs_payload={
                    "total_count": len(self.jobs) + 1,
                    "jobs": self.jobs,
                },
                artifacts_payload={
                    "total_count": len(self.artifacts),
                    "artifacts": self.artifacts,
                },
                download_artifact=self.archives.__getitem__,
            )

    def test_rejects_altered_artifact_and_binary(self) -> None:
        artifact_id = int(self.artifacts[0]["id"])
        self.artifacts[0]["digest"] = "sha256:" + "0" * 64
        with self.assertRaisesRegex(evidence.EvidenceError, "artifact digest"):
            self.collect()
        archive = self.archives[artifact_id]
        self.artifacts[0]["digest"] = f"sha256:{sha256(archive)}"
        with zipfile.ZipFile(io.BytesIO(archive)) as source:
            manifest = json.loads(source.read("evidence.json"))
            binary_path = manifest["binaries"][0]["path"]
            binary = source.read(binary_path)
        manifest["binaries"][0]["sha256"] = "0" * 64
        altered = self.archive(manifest, {binary_path: binary})
        self.archives[artifact_id] = altered
        self.artifacts[0]["digest"] = f"sha256:{sha256(altered)}"
        with self.assertRaisesRegex(evidence.EvidenceError, "binary digest"):
            self.collect()

    def test_rejects_duplicate_scenarios_and_weak_e2e_topology(self) -> None:
        artifact_id = int(self.artifacts[0]["id"])
        with zipfile.ZipFile(io.BytesIO(self.archives[artifact_id])) as source:
            manifest = json.loads(source.read("evidence.json"))
            binary_path = manifest["binaries"][0]["path"]
            binary = source.read(binary_path)
        manifest["scenarios"].append(manifest["scenarios"][0])
        altered = self.archive(manifest, {binary_path: binary})
        self.archives[artifact_id] = altered
        self.artifacts[0]["digest"] = f"sha256:{sha256(altered)}"
        with self.assertRaisesRegex(evidence.EvidenceError, "duplicates"):
            self.collect()

        manifest["scenarios"] = ["transfer.explicit_accept"]
        manifest["runtime"] = {
            "process_count": 1,
            "transport": "in_memory",
            "authenticated": False,
            "packaged": False,
        }
        manifest["binaries"] = []
        altered = self.archive(manifest, {})
        self.archives[artifact_id] = altered
        self.artifacts[0]["digest"] = f"sha256:{sha256(altered)}"
        with self.assertRaisesRegex(
            evidence.EvidenceError,
            "packaged E2E",
        ):
            self.collect()

    def test_rejects_generic_verify_for_specialized_evidence(self) -> None:
        contract = self.plan["requirements"][0]["criteria"][0]["evidence"][0]
        contract["gate"] = "verify"
        with self.assertRaisesRegex(evidence.EvidenceError, "generic verify"):
            self.collect()

    def test_bundle_rejects_unknown_duplicate_and_stale_data(self) -> None:
        bundle = self.collect()
        unknown = copy.deepcopy(bundle)
        unknown["task_authored"] = True
        errors = evidence.validate_bundle(
            self.task,
            self.record,
            self.plan,
            unknown,
            self.gates,
            SOURCE_SHA,
        )
        self.assertTrue(any("unknown fields" in error for error in errors))

        duplicate = copy.deepcopy(bundle)
        duplicate["criteria"].append(copy.deepcopy(duplicate["criteria"][0]))
        errors = evidence.validate_bundle(
            self.task,
            self.record,
            self.plan,
            duplicate,
            self.gates,
            SOURCE_SHA,
        )
        self.assertTrue(any("duplicate criterion" in error for error in errors))

        stale = copy.deepcopy(bundle)
        stale["source_sha"] = "c" * 40
        errors = evidence.validate_bundle(
            self.task,
            self.record,
            self.plan,
            stale,
            self.gates,
            SOURCE_SHA,
        )
        self.assertTrue(any("source_sha" in error for error in errors))

    def test_acceptance_task_recollects_producer_contract(self) -> None:
        self.task["id"] = "XT-084"
        self.task["delivery_role"] = "acceptance"
        self.record["id"] = "XT-084"
        self.record["test_contract"]["criterion_ids"] = [
            "CRIT-FIXTURE-EXACT-BYTES"
        ]
        self.run["head_branch"] = "ci/XT-084"
        bundle = self.collect()
        proof = bundle["criteria"][0]["evidence"][0]
        self.assertEqual(proof["producer_task"], "XT-083")
        self.assertEqual(bundle["source_sha"], SOURCE_SHA)

    def test_record_evidence_is_generated_only_for_done_schema_v4(self) -> None:
        bundle = self.collect()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            plan_path = root / ".agents" / "plans" / "DP-FIXTURE.json"
            plan_path.parent.mkdir(parents=True)
            plan_path.write_text(json.dumps(self.plan), encoding="utf-8")
            accepted = copy.deepcopy(self.record)
            accepted["state"] = "done"
            accepted["criterion_evidence"] = bundle
            self.assertEqual(
                evidence.validate_record_evidence(
                    root,
                    self.task,
                    accepted,
                    self.gates,
                ),
                [],
            )
            accepted["state"] = "integrated"
            errors = evidence.validate_record_evidence(
                root,
                self.task,
                accepted,
                self.gates,
            )
            self.assertTrue(any("only at acceptance" in error for error in errors))
            accepted["state"] = "done"
            accepted["schema_version"] = 3
            errors = evidence.validate_record_evidence(
                root,
                self.task,
                accepted,
                self.gates,
            )
            self.assertTrue(any("only for schema 4" in error for error in errors))

    def test_ci_matrix_expands_contract_and_ignores_legacy_records(self) -> None:
        matrix = evidence.ci_matrix(
            self.task,
            self.record,
            self.plan,
            self.gates,
        )
        self.assertEqual(len(matrix["include"]), 4)
        self.assertEqual(
            {
                (entry["runner"], entry["platform"], entry["role"])
                for entry in matrix["include"]
            },
            {
                ("ubuntu-latest", "linux", "sender"),
                ("ubuntu-latest", "linux", "receiver"),
                ("macos-latest", "macos", "sender"),
                ("macos-latest", "macos", "receiver"),
            },
        )
        legacy = copy.deepcopy(self.record)
        legacy["schema_version"] = 3
        self.assertEqual(
            evidence.ci_matrix(
                self.task,
                legacy,
                self.plan,
                self.gates,
            ),
            {"include": []},
        )

    def test_non_producer_implementation_has_no_remote_evidence_jobs(self) -> None:
        self.task["id"] = "XT-085"
        self.record["id"] = "XT-085"
        criterion = self.plan["requirements"][0]["criteria"][0]
        criterion["implementation_tasks"].append("XT-085")
        self.assertEqual(
            evidence.ci_matrix(
                self.task,
                self.record,
                self.plan,
                self.gates,
            ),
            {"include": []},
        )


if __name__ == "__main__":
    unittest.main()
