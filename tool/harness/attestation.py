#!/usr/bin/env python3
"""Harness V2 workflow and acceptance attestation validation."""

from __future__ import annotations

import re
from typing import Any, Dict, List, Mapping, Optional, Sequence

import git_ops
from model import ContractSet, SHA256, canonical_sha256


SHA = re.compile(r"^[0-9a-f]{40}$")
WORKFLOW_FIELDS = {
    "repository",
    "workflow_path",
    "workflow_blob",
    "run_id",
    "run_attempt",
    "head_sha",
    "head_branch",
    "event",
    "conclusion",
    "jobs",
    "artifacts",
}
JOB_FIELDS = {"name", "conclusion"}
ARTIFACT_FIELDS = {
    "name",
    "source_sha",
    "sha256",
    "platform",
    "gate_ids",
    "gate_attestations",
    "criterion_ids",
    "criterion_evidence",
}
ACCEPTANCE_FIELDS = {
    "schema_version",
    "task_id",
    "submission_sha256",
    "candidate_sha",
    "candidate_tree",
    "integration_base",
    "payload_patch_sha256",
    "workflow",
    "required_jobs",
    "required_artifacts",
    "required_gate_attestations",
    "criterion_evidence",
    "skipped_jobs",
    "created_by",
    "created_at",
}


class AttestationError(RuntimeError):
    """Raised when workflow or publication evidence is incomplete."""


def criterion_evidence_digest(
    contracts: ContractSet,
    criterion_id: str,
    source_sha: str,
    gate_attestations: Sequence[str],
) -> str:
    documents = {
        criterion["id"]: criterion
        for plan in contracts.plans.values()
        for requirement in plan["requirements"]
        for criterion in requirement["criteria"]
    }
    if criterion_id not in documents:
        raise AttestationError("unknown criterion {}".format(criterion_id))
    return canonical_sha256(
        {
            "criterion_id": criterion_id,
            "criterion_sha256": canonical_sha256(documents[criterion_id]),
            "source_sha": source_sha,
            "gate_attestations": list(gate_attestations),
        }
    )


def _string(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value.strip() or value != value.strip():
        raise AttestationError("{} must be a non-empty string".format(label))
    return value


def _string_list(value: Any, label: str, allow_empty: bool = False) -> List[str]:
    if not isinstance(value, list):
        raise AttestationError("{} must be an array".format(label))
    result = [_string(item, "{}[]".format(label)) for item in value]
    if not allow_empty and not result:
        raise AttestationError("{} must not be empty".format(label))
    if len(result) != len(set(result)):
        raise AttestationError("{} contains duplicates".format(label))
    return result


def validate_workflow_evidence(
    value: Any,
    *,
    repository: str,
    workflow_path: str,
    workflow_blob: str,
    candidate_sha: str,
    candidate_branch: str,
    required_jobs: Sequence[str],
    required_artifacts: Sequence[str],
) -> Dict[str, Any]:
    if not isinstance(value, dict) or set(value) != WORKFLOW_FIELDS:
        raise AttestationError("workflow evidence has invalid fields")
    evidence = dict(value)
    expected = {
        "repository": repository,
        "workflow_path": workflow_path,
        "workflow_blob": workflow_blob,
        "head_sha": candidate_sha,
        "head_branch": candidate_branch,
    }
    for field, wanted in expected.items():
        if evidence[field] != wanted:
            raise AttestationError(
                "workflow evidence {} does not match candidate".format(field)
            )
    if SHA.fullmatch(evidence["head_sha"]) is None:
        raise AttestationError("workflow head SHA is invalid")
    if len(evidence["workflow_blob"]) != 40:
        raise AttestationError("workflow blob is invalid")
    if (
        not isinstance(evidence["run_id"], int)
        or isinstance(evidence["run_id"], bool)
        or evidence["run_id"] < 1
        or not isinstance(evidence["run_attempt"], int)
        or isinstance(evidence["run_attempt"], bool)
        or evidence["run_attempt"] < 1
    ):
        raise AttestationError("workflow run identity is invalid")
    if evidence["event"] != "push" or evidence["conclusion"] != "success":
        raise AttestationError("workflow did not complete successfully for a push")
    jobs = evidence["jobs"]
    if not isinstance(jobs, list):
        raise AttestationError("workflow jobs must be an array")
    job_results: Dict[str, str] = {}
    for raw in jobs:
        if not isinstance(raw, dict) or set(raw) != JOB_FIELDS:
            raise AttestationError("workflow job has invalid fields")
        name = _string(raw["name"], "workflow job name")
        if name in job_results:
            raise AttestationError("workflow job names are duplicated")
        conclusion = _string(raw["conclusion"], "workflow job conclusion")
        job_results[name] = conclusion
    missing_jobs = sorted(set(required_jobs) - set(job_results))
    if missing_jobs:
        raise AttestationError(
            "required workflow jobs are missing: {}".format(", ".join(missing_jobs))
        )
    skipped_jobs = sorted(
        name for name, conclusion in job_results.items() if conclusion == "skipped"
    )
    if skipped_jobs:
        raise AttestationError(
            "workflow contains skipped jobs: {}".format(", ".join(skipped_jobs))
        )
    rejected = sorted(
        name for name in required_jobs if job_results.get(name) != "success"
    )
    if rejected:
        raise AttestationError(
            "required workflow jobs did not succeed: {}".format(", ".join(rejected))
        )
    artifacts = evidence["artifacts"]
    if not isinstance(artifacts, list):
        raise AttestationError("workflow artifacts must be an array")
    artifact_results: Dict[str, Dict[str, Any]] = {}
    for raw in artifacts:
        if not isinstance(raw, dict) or set(raw) != ARTIFACT_FIELDS:
            raise AttestationError("workflow artifact has invalid fields")
        name = _string(raw["name"], "artifact name")
        if name in artifact_results:
            raise AttestationError("artifact names are duplicated")
        if raw["source_sha"] != candidate_sha:
            raise AttestationError("artifact {} has stale source SHA".format(name))
        if (
            not isinstance(raw["sha256"], str)
            or SHA256.fullmatch(raw["sha256"]) is None
        ):
            raise AttestationError("artifact {} digest is invalid".format(name))
        if raw["platform"] not in {"linux", "macos", "windows"}:
            raise AttestationError("artifact {} platform is invalid".format(name))
        gate_ids = _string_list(raw["gate_ids"], "artifact {} gate IDs".format(name))
        criterion_ids = _string_list(
            raw["criterion_ids"], "artifact {} criterion IDs".format(name)
        )
        for field in ("gate_attestations", "criterion_evidence"):
            digests = _string_list(raw[field], "artifact {} {}".format(name, field))
            if any(SHA256.fullmatch(digest) is None for digest in digests):
                raise AttestationError(
                    "artifact {} {} digest is invalid".format(name, field)
                )
        if len(gate_ids) != len(raw["gate_attestations"]):
            raise AttestationError(
                "artifact {} Gate identities are incomplete".format(name)
            )
        if len(criterion_ids) != len(raw["criterion_evidence"]):
            raise AttestationError(
                "artifact {} criterion identities are incomplete".format(name)
            )
        artifact_results[name] = dict(raw)
    missing_artifacts = sorted(set(required_artifacts) - set(artifact_results))
    if missing_artifacts:
        raise AttestationError(
            "required artifacts are missing: {}".format(", ".join(missing_artifacts))
        )
    return evidence


def validate_criterion_evidence(
    contracts: ContractSet,
    workflow: Mapping[str, Any],
    *,
    candidate_sha: str,
    required_artifacts: Sequence[str],
    criterion_ids: Sequence[str],
    gate_attestations: Sequence[str],
    criterion_evidence: Sequence[str],
) -> None:
    artifacts = {artifact["name"]: artifact for artifact in workflow["artifacts"]}
    normalized_gates = sorted(
        {
            digest
            for name in required_artifacts
            for digest in artifacts[name]["gate_attestations"]
        }
    )
    normalized_criteria = sorted(
        {
            digest
            for name in required_artifacts
            for digest in artifacts[name]["criterion_evidence"]
        }
    )
    if sorted(set(gate_attestations)) != normalized_gates:
        raise AttestationError("published Gate evidence is incomplete")
    if sorted(set(criterion_evidence)) != normalized_criteria:
        raise AttestationError("published criterion evidence is incomplete")
    for name in required_artifacts:
        artifact = artifacts[name]
        criterion_map = dict(
            zip(
                artifact["criterion_ids"],
                artifact["criterion_evidence"],
            )
        )
        for criterion_id in criterion_ids:
            expected = criterion_evidence_digest(
                contracts,
                criterion_id,
                candidate_sha,
                artifact["gate_attestations"],
            )
            if criterion_map.get(criterion_id) != expected:
                raise AttestationError(
                    "artifact {} lacks bound criterion evidence".format(name)
                )


def create_acceptance_attestation(
    *,
    task_id: str,
    submission: Mapping[str, Any],
    candidate_sha: str,
    candidate_tree: str,
    integration_base: str,
    payload_patch_sha256: str,
    workflow: Mapping[str, Any],
    required_jobs: Sequence[str],
    required_artifacts: Sequence[str],
    gate_attestations: Sequence[str],
    criterion_evidence: Sequence[str],
    actor: Mapping[str, Any],
    created_at: str,
) -> Dict[str, Any]:
    for label, value in (
        ("candidate_sha", candidate_sha),
        ("candidate_tree", candidate_tree),
        ("integration_base", integration_base),
    ):
        if SHA.fullmatch(value) is None:
            raise AttestationError("{} is invalid".format(label))
    if SHA256.fullmatch(payload_patch_sha256) is None:
        raise AttestationError("payload patch digest is invalid")
    gate_values = _string_list(list(gate_attestations), "gate attestations")
    criterion_values = _string_list(list(criterion_evidence), "criterion evidence")
    for digest in gate_values + criterion_values:
        if SHA256.fullmatch(digest) is None:
            raise AttestationError("attestation digest is invalid")
    return {
        "schema_version": 1,
        "task_id": task_id,
        "submission_sha256": canonical_sha256(submission),
        "candidate_sha": candidate_sha,
        "candidate_tree": candidate_tree,
        "integration_base": integration_base,
        "payload_patch_sha256": payload_patch_sha256,
        "workflow": dict(workflow),
        "required_jobs": list(required_jobs),
        "required_artifacts": list(required_artifacts),
        "required_gate_attestations": gate_values,
        "criterion_evidence": criterion_values,
        "skipped_jobs": [],
        "created_by": dict(actor),
        "created_at": created_at,
    }


def validate_acceptance_attestation(
    value: Any, task_id: str, candidate_sha: str
) -> Dict[str, Any]:
    if not isinstance(value, dict) or set(value) != ACCEPTANCE_FIELDS:
        raise AttestationError("acceptance attestation has invalid fields")
    attestation = dict(value)
    if (
        attestation["schema_version"] != 1
        or attestation["task_id"] != task_id
        or attestation["candidate_sha"] != candidate_sha
    ):
        raise AttestationError("acceptance attestation identity is invalid")
    if attestation["skipped_jobs"] != []:
        raise AttestationError("acceptance attestation contains skipped jobs")
    if SHA256.fullmatch(attestation["submission_sha256"]) is None:
        raise AttestationError("submission digest is invalid")
    if SHA256.fullmatch(attestation["payload_patch_sha256"]) is None:
        raise AttestationError("payload patch digest is invalid")
    if SHA.fullmatch(attestation["candidate_tree"]) is None:
        raise AttestationError("candidate tree is invalid")
    if SHA.fullmatch(attestation["integration_base"]) is None:
        raise AttestationError("integration base is invalid")
    return attestation


class AcceptanceStore:
    def __init__(self, contracts: ContractSet, remote: Optional[str] = None) -> None:
        self.contracts = contracts
        self.root = contracts.root
        self.remote = remote
        self.prefix = contracts.manifest["ref_namespaces"]["attest"]

    def ref(self, task_id: str, candidate_sha: str) -> str:
        return "{}acceptance/{}/{}".format(self.prefix, task_id, candidate_sha)

    def write(self, attestation: Mapping[str, Any]) -> str:
        task_id = str(attestation["task_id"])
        candidate_sha = str(attestation["candidate_sha"])
        validate_acceptance_attestation(attestation, task_id, candidate_sha)
        ref = self.ref(task_id, candidate_sha)
        existing = git_ops.ref_sha(self.root, ref)
        if existing is not None:
            previous = git_ops.read_json_object(self.root, existing, "attestation.json")
            if previous != attestation:
                raise AttestationError("acceptance ref already contains other evidence")
            if self.remote is not None:
                remote_sha = git_ops.remote_ref_sha(self.root, self.remote, ref)
                if remote_sha is None:
                    try:
                        git_ops.push_ref_cas(
                            self.root,
                            self.remote,
                            existing,
                            ref,
                            None,
                        )
                    except git_ops.GitError as error:
                        raise AttestationError(str(error)) from error
                elif remote_sha != existing:
                    raise AttestationError(
                        "remote acceptance ref contains other evidence"
                    )
            return existing
        commit = git_ops.commit_json(
            self.root,
            attestation,
            "{} acceptance attestation".format(task_id),
            filename="attestation.json",
        )
        git_ops.update_ref_cas(self.root, ref, commit, None)
        if self.remote is not None:
            try:
                git_ops.push_ref_cas(self.root, self.remote, commit, ref, None)
            except git_ops.GitError as error:
                git_ops.run_git(self.root, "update-ref", "-d", ref, commit, check=False)
                raise AttestationError(str(error)) from error
        return commit

    def maybe_read(
        self, task_id: str, candidate_sha: str
    ) -> Optional[Mapping[str, Any]]:
        ref = self.ref(task_id, candidate_sha)
        if self.remote is not None:
            remote_sha = git_ops.remote_ref_sha(self.root, self.remote, ref)
            if remote_sha is None:
                return None
            try:
                git_ops.fetch_immutable_ref(self.root, self.remote, ref)
            except git_ops.GitError as error:
                raise AttestationError(str(error)) from error
        commit = git_ops.ref_sha(self.root, ref)
        if commit is None:
            return None
        value = git_ops.read_json_object(self.root, commit, "attestation.json")
        return validate_acceptance_attestation(value, task_id, candidate_sha)

    def read(self, task_id: str, candidate_sha: str) -> Mapping[str, Any]:
        value = self.maybe_read(task_id, candidate_sha)
        if value is None:
            raise AttestationError("acceptance attestation is missing")
        return value
