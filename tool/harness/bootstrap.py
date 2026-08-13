#!/usr/bin/env python3
"""One-time Harness V2 bootstrap acceptance and immutable evidence refs."""

from __future__ import annotations

import re
from typing import Any, Dict, Mapping, Optional, Sequence

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
    "artifact_id",
    "source_sha",
    "source_tree",
    "sha256",
    "platform",
    "plan_sha256",
    "gate_ids",
    "gate_attestations",
    "skipped",
}
BOOTSTRAP_FIELDS = {
    "schema_version",
    "kind",
    "candidate_sha",
    "candidate_tree",
    "integration_base",
    "plan_sha256",
    "workflow",
    "required_jobs",
    "required_artifacts",
    "required_gate_attestations",
    "platforms",
    "skipped_jobs",
    "created_by",
    "created_at",
}


class BootstrapError(RuntimeError):
    """Raised when the one-time cutover evidence is incomplete."""


def _strings(value: Any, label: str) -> list[str]:
    if (
        not isinstance(value, list)
        or not value
        or any(not isinstance(item, str) or not item for item in value)
        or len(value) != len(set(value))
    ):
        raise BootstrapError("{} must contain unique strings".format(label))
    return list(value)


def validate_workflow_evidence(
    value: Any,
    *,
    repository: str,
    workflow_path: str,
    workflow_blob: str,
    candidate_sha: str,
    candidate_tree: str,
    candidate_branch: str,
    required_jobs: Sequence[str],
    required_artifacts: Sequence[str],
    expected_plan_sha256: str,
    expected_gates: Mapping[str, Sequence[str]],
) -> Dict[str, Any]:
    if not isinstance(value, dict) or set(value) != WORKFLOW_FIELDS:
        raise BootstrapError("bootstrap workflow evidence has invalid fields")
    evidence = dict(value)
    expected_identity = {
        "repository": repository,
        "workflow_path": workflow_path,
        "workflow_blob": workflow_blob,
        "head_sha": candidate_sha,
        "head_branch": candidate_branch,
        "event": "push",
        "conclusion": "success",
    }
    for field, expected in expected_identity.items():
        if evidence.get(field) != expected:
            raise BootstrapError(
                "bootstrap workflow {} does not match candidate".format(field)
            )
    if (
        SHA.fullmatch(candidate_sha) is None
        or SHA.fullmatch(candidate_tree) is None
        or SHA.fullmatch(workflow_blob) is None
        or not isinstance(evidence["run_id"], int)
        or isinstance(evidence["run_id"], bool)
        or evidence["run_id"] < 1
        or not isinstance(evidence["run_attempt"], int)
        or isinstance(evidence["run_attempt"], bool)
        or evidence["run_attempt"] < 1
    ):
        raise BootstrapError("bootstrap workflow identity is invalid")

    jobs = evidence["jobs"]
    if not isinstance(jobs, list):
        raise BootstrapError("bootstrap jobs must be an array")
    job_results: Dict[str, str] = {}
    for job in jobs:
        if not isinstance(job, dict) or set(job) != JOB_FIELDS:
            raise BootstrapError("bootstrap job has invalid fields")
        name = job["name"]
        conclusion = job["conclusion"]
        if (
            not isinstance(name, str)
            or not name
            or name in job_results
            or not isinstance(conclusion, str)
        ):
            raise BootstrapError("bootstrap job identity is invalid")
        job_results[name] = conclusion
    skipped = sorted(
        name for name, conclusion in job_results.items() if conclusion == "skipped"
    )
    if skipped:
        raise BootstrapError(
            "bootstrap workflow contains skipped jobs: {}".format(", ".join(skipped))
        )
    rejected = sorted(
        name for name in required_jobs if job_results.get(name) != "success"
    )
    if rejected:
        raise BootstrapError(
            "bootstrap jobs did not succeed: {}".format(", ".join(rejected))
        )

    artifacts = evidence["artifacts"]
    if not isinstance(artifacts, list):
        raise BootstrapError("bootstrap artifacts must be an array")
    by_name: Dict[str, Dict[str, Any]] = {}
    plan_digests = set()
    platforms = set()
    for artifact in artifacts:
        if not isinstance(artifact, dict) or set(artifact) != ARTIFACT_FIELDS:
            raise BootstrapError("bootstrap artifact has invalid fields")
        name = artifact["name"]
        platform = artifact["platform"]
        if (
            not isinstance(name, str)
            or not name
            or name in by_name
            or platform not in expected_gates
            or platform in platforms
        ):
            raise BootstrapError("bootstrap artifact identity is invalid")
        if (
            artifact["source_sha"] != candidate_sha
            or artifact["source_tree"] != candidate_tree
            or artifact["skipped"] is not False
            or not isinstance(artifact["artifact_id"], int)
            or isinstance(artifact["artifact_id"], bool)
            or artifact["artifact_id"] < 1
            or not isinstance(artifact["sha256"], str)
            or SHA256.fullmatch(artifact["sha256"]) is None
            or not isinstance(artifact["plan_sha256"], str)
            or SHA256.fullmatch(artifact["plan_sha256"]) is None
            or artifact["plan_sha256"] != expected_plan_sha256
        ):
            raise BootstrapError("bootstrap artifact binding is invalid")
        gate_ids = _strings(artifact["gate_ids"], "bootstrap Gate IDs")
        gate_attestations = _strings(
            artifact["gate_attestations"], "bootstrap Gate attestations"
        )
        if (
            gate_ids != list(expected_gates[platform])
            or len(gate_ids) != len(gate_attestations)
            or any(SHA256.fullmatch(digest) is None for digest in gate_attestations)
        ):
            raise BootstrapError(
                "{} bootstrap Gate evidence is incomplete".format(platform)
            )
        plan_digests.add(artifact["plan_sha256"])
        platforms.add(platform)
        by_name[name] = dict(artifact)
    if set(required_artifacts) != set(by_name):
        raise BootstrapError("bootstrap artifact set is incomplete")
    if platforms != set(expected_gates):
        raise BootstrapError("bootstrap platform matrix is incomplete")
    if len(plan_digests) != 1:
        raise BootstrapError("bootstrap artifacts use different Gate plans")
    return evidence


def create_attestation(
    *,
    candidate_sha: str,
    candidate_tree: str,
    integration_base: str,
    workflow: Mapping[str, Any],
    required_jobs: Sequence[str],
    required_artifacts: Sequence[str],
    actor: Mapping[str, Any],
    created_at: str,
) -> Dict[str, Any]:
    artifacts = list(workflow["artifacts"])
    gate_attestations = sorted(
        {digest for artifact in artifacts for digest in artifact["gate_attestations"]}
    )
    plan_digests = {artifact["plan_sha256"] for artifact in artifacts}
    if len(plan_digests) != 1:
        raise BootstrapError("bootstrap artifacts use different Gate plans")
    value = {
        "schema_version": 1,
        "kind": "bootstrap_acceptance",
        "candidate_sha": candidate_sha,
        "candidate_tree": candidate_tree,
        "integration_base": integration_base,
        "plan_sha256": plan_digests.pop(),
        "workflow": dict(workflow),
        "required_jobs": list(required_jobs),
        "required_artifacts": list(required_artifacts),
        "required_gate_attestations": gate_attestations,
        "platforms": sorted(artifact["platform"] for artifact in artifacts),
        "skipped_jobs": [],
        "created_by": dict(actor),
        "created_at": created_at,
    }
    return validate_attestation(value, candidate_sha)


def validate_attestation(value: Any, candidate_sha: str) -> Dict[str, Any]:
    if not isinstance(value, dict) or set(value) != BOOTSTRAP_FIELDS:
        raise BootstrapError("bootstrap acceptance has invalid fields")
    result = dict(value)
    if (
        result["schema_version"] != 1
        or result["kind"] != "bootstrap_acceptance"
        or result["candidate_sha"] != candidate_sha
        or SHA.fullmatch(candidate_sha) is None
        or SHA.fullmatch(result["candidate_tree"]) is None
        or SHA.fullmatch(result["integration_base"]) is None
        or SHA256.fullmatch(result["plan_sha256"]) is None
        or result["platforms"] != ["linux", "macos", "windows"]
        or result["skipped_jobs"] != []
    ):
        raise BootstrapError("bootstrap acceptance identity is invalid")
    _strings(result["required_jobs"], "bootstrap required jobs")
    _strings(result["required_artifacts"], "bootstrap required artifacts")
    digests = _strings(
        result["required_gate_attestations"], "bootstrap Gate attestations"
    )
    if any(SHA256.fullmatch(digest) is None for digest in digests):
        raise BootstrapError("bootstrap Gate attestation digest is invalid")
    if not isinstance(result["workflow"], dict):
        raise BootstrapError("bootstrap workflow evidence is invalid")
    if not isinstance(result["created_by"], dict):
        raise BootstrapError("bootstrap actor is invalid")
    if not isinstance(result["created_at"], str) or not result["created_at"].endswith(
        "Z"
    ):
        raise BootstrapError("bootstrap timestamp is invalid")
    return result


class AcceptanceStore:
    def __init__(self, contracts: ContractSet, remote: Optional[str]) -> None:
        self.root = contracts.root
        self.remote = remote
        self.prefix = contracts.manifest["ref_namespaces"]["attest"]
        owner = contracts.manifest["project_owner"]
        self.owner = {
            "kind": "project-owner",
            "id": owner["id"],
            "name": owner["name"],
            "email": owner["email"],
        }

    def ref(self, candidate_sha: str) -> str:
        return "{}bootstrap/{}".format(self.prefix, candidate_sha)

    def write(self, value: Mapping[str, Any]) -> str:
        candidate_sha = str(value["candidate_sha"])
        validate_attestation(value, candidate_sha)
        if value["created_by"] != self.owner:
            raise BootstrapError(
                "bootstrap acceptance actor is not the configured project owner"
            )
        ref = self.ref(candidate_sha)
        remote_sha = None
        if self.remote is not None:
            remote_sha = git_ops.remote_ref_sha(self.root, self.remote, ref)
            if remote_sha is not None:
                try:
                    git_ops.fetch_immutable_ref(self.root, self.remote, ref)
                except git_ops.GitError as error:
                    raise BootstrapError(str(error)) from error
        existing = git_ops.ref_sha(self.root, ref)
        if existing is not None:
            previous = git_ops.read_json_object(self.root, existing, "attestation.json")
            if canonical_sha256(previous) != canonical_sha256(value):
                raise BootstrapError("bootstrap ref already contains other evidence")
            if self.remote is not None and remote_sha is None:
                try:
                    git_ops.push_ref_cas(
                        self.root,
                        self.remote,
                        existing,
                        ref,
                        None,
                    )
                except git_ops.GitError as error:
                    raise BootstrapError(str(error)) from error
            return existing
        commit = git_ops.commit_json(
            self.root,
            value,
            "Harness V2 bootstrap acceptance",
            filename="attestation.json",
        )
        try:
            git_ops.update_ref_cas(self.root, ref, commit, None)
            if self.remote is not None:
                git_ops.push_ref_cas(self.root, self.remote, commit, ref, None)
        except git_ops.GitError as error:
            git_ops.run_git(self.root, "update-ref", "-d", ref, commit, check=False)
            raise BootstrapError(str(error)) from error
        return commit

    def read(self, candidate_sha: str) -> Dict[str, Any]:
        ref = self.ref(candidate_sha)
        if self.remote is not None:
            try:
                git_ops.fetch_immutable_ref(self.root, self.remote, ref)
            except git_ops.GitError as error:
                raise BootstrapError(str(error)) from error
        commit = git_ops.ref_sha(self.root, ref)
        if commit is None:
            raise BootstrapError("bootstrap acceptance ref is missing")
        value = git_ops.read_json_object(self.root, commit, "attestation.json")
        result = validate_attestation(value, candidate_sha)
        if result["created_by"] != self.owner:
            raise BootstrapError(
                "bootstrap acceptance actor is not the configured project owner"
            )
        return result


def publish_candidate(
    contracts: ContractSet,
    remote: str,
    value: Mapping[str, Any],
    queue_ref: Optional[str] = None,
) -> str:
    candidate_sha = str(value["candidate_sha"])
    attestation = validate_attestation(value, candidate_sha)
    owner = contracts.manifest["project_owner"]
    if attestation["created_by"] != {
        "kind": "project-owner",
        "id": owner["id"],
        "name": owner["name"],
        "email": owner["email"],
    }:
        raise BootstrapError(
            "bootstrap acceptance actor is not the configured project owner"
        )
    candidate_tree = git_ops.current_tree(contracts.root, candidate_sha)
    if candidate_tree != attestation["candidate_tree"]:
        raise BootstrapError("bootstrap candidate tree changed")
    protected_ref = "refs/heads/{}".format(contracts.manifest["integration_branch"])
    remote_head = git_ops.remote_ref_sha(contracts.root, remote, protected_ref)
    if remote_head == candidate_sha:
        _reclaim_queue_ref(contracts, remote, queue_ref, candidate_sha)
        return "already_published"
    if remote_head != attestation["integration_base"]:
        raise BootstrapError(
            "protected branch moved: expected {}, found {}".format(
                attestation["integration_base"],
                remote_head or "<missing>",
            )
        )
    if not git_ops.is_ancestor(
        contracts.root,
        attestation["integration_base"],
        candidate_sha,
    ):
        raise BootstrapError("bootstrap candidate is not a fast-forward")
    try:
        git_ops.push_ref_cas(
            contracts.root,
            remote,
            candidate_sha,
            protected_ref,
            attestation["integration_base"],
        )
    except git_ops.GitError as error:
        raise BootstrapError(str(error)) from error
    published = git_ops.remote_ref_sha(contracts.root, remote, protected_ref)
    if published != candidate_sha:
        raise BootstrapError("protected branch changed after bootstrap publication")
    _reclaim_queue_ref(contracts, remote, queue_ref, candidate_sha)
    return "published"


def _reclaim_queue_ref(
    contracts: ContractSet,
    remote: str,
    queue_ref: Optional[str],
    candidate_sha: str,
) -> None:
    if queue_ref is None:
        return
    prefix = contracts.manifest["ref_namespaces"]["queue"] + "bootstrap/"
    if not queue_ref.startswith(prefix):
        raise BootstrapError("bootstrap queue ref is outside the transient namespace")
    try:
        git_ops.delete_remote_ref_cas(
            contracts.root,
            remote,
            queue_ref,
            candidate_sha,
        )
        git_ops.delete_ref_cas(contracts.root, queue_ref, candidate_sha)
    except git_ops.GitError as error:
        raise BootstrapError(
            "bootstrap published but queue cleanup failed: {}".format(error)
        ) from error
