#!/usr/bin/env python3
"""Harness V2 static contract loading and validation."""

from __future__ import annotations

import copy
import fnmatch
import hashlib
import json
import re
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, FrozenSet, List, Mapping, Sequence, Set, Tuple


PLAN_ID = re.compile(r"^DP-[A-Z0-9-]+$")
TASK_ID = re.compile(r"^XT-[0-9]{3,}$")
REQUIREMENT_ID = re.compile(r"^REQ-[A-Z0-9-]+$")
CRITERION_ID = re.compile(r"^CRIT-[A-Z0-9-]+$")
GATE_ID = re.compile(r"^[a-z][a-z0-9_]*$")
SHA256 = re.compile(r"^[0-9a-f]{64}$")

RISK_DIMENSIONS = (
    "functionality",
    "security",
    "performance",
    "compatibility",
    "concurrency",
    "platform",
    "persistence",
)
RISK_RANK = {
    "none": 0,
    "low": 1,
    "medium": 2,
    "high": 3,
    "critical": 4,
}
TASK_TYPES = {
    "feature",
    "bugfix",
    "refactor",
    "investigation",
    "test",
    "governance",
    "documentation",
    "acceptance",
}
TDD_MODE_BY_TYPE = {
    "feature": {"red_green"},
    "bugfix": {"regression"},
    "refactor": {"equivalence"},
    "investigation": {"bounded_evidence"},
    "test": {"mutation"},
    "governance": {"adversarial"},
    "documentation": {"not_required"},
    "acceptance": {"evidence_closure"},
}
ARCHITECTURE_MODES = {"none", "add", "replace", "remove", "refactor"}
CACHE_MODES = {"disabled", "success_only"}
PLAN_STATUSES = {"draft", "approved"}
PLATFORMS = {"local", "linux", "macos", "windows"}
EVIDENCE_PLATFORMS = {"linux", "macos", "windows"}

MANIFEST_FIELDS = {
    "schema_version",
    "harness_version",
    "project",
    "integration_branch",
    "project_owner",
    "ref_namespaces",
}
PROJECT_OWNER_FIELDS = {"id", "name", "email"}
REF_NAMESPACE_FIELDS = {
    "approve",
    "state",
    "submit",
    "queue",
    "attest",
    "archive",
}
PLAN_FIELDS = {
    "schema_version",
    "id",
    "title",
    "status",
    "source",
    "requirements",
    "approval",
}
PLAN_SOURCE_FIELDS = {"kind", "path"}
REQUIREMENT_FIELDS = {
    "id",
    "statement",
    "criteria",
    "implementation_tasks",
    "acceptance_owner",
}
CRITERION_FIELDS = {
    "id",
    "statement",
    "negative_definitions",
    "evidence",
}
EVIDENCE_FIELDS = {
    "gates",
    "scenarios",
    "topology",
    "platforms",
    "roles",
    "allow_skipped",
}
APPROVAL_FIELDS = {"approved_by", "approved_at", "content_sha256"}
TASK_FIELDS = {
    "schema_version",
    "id",
    "title",
    "plan",
    "criteria",
    "depends_on",
    "owned_paths",
    "type",
    "workstream",
    "risk",
    "tdd",
    "delivery",
}
TDD_FIELDS = {
    "mode",
    "gate",
    "proof_paths",
    "oracle_paths",
    "failure_fingerprints",
}
DELIVERY_FIELDS = {
    "commit_type",
    "scope",
    "summary",
    "architecture_change",
}
ARCHITECTURE_CHANGE_FIELDS = {
    "mode",
    "modules",
    "supersedes",
    "temporary_leases",
    "retires_leases",
}
SUPERSEDES_FIELDS = {"paths", "symbols", "targets"}
SYMBOL_FIELDS = {"path", "name"}
LEASE_FIELDS = {"id", "path", "remove_by_task", "reason"}
GATE_POLICY_FIELDS = {"schema_version", "resource_groups", "gates"}
RESOURCE_GROUP_FIELDS = {"max_parallel"}
GATE_FIELDS = {
    "command",
    "aggregate",
    "inputs",
    "resource_group",
    "platforms",
    "cache",
}
COMMAND_FIELDS = {"argv", "timeout_seconds", "environment"}
RISK_ROUTING_FIELDS = {"schema_version", "path_rules", "phase_minimums"}
PATH_RULE_FIELDS = {"paths", "minimum_risk", "required_gates"}
MIGRATION_FIELDS = {
    "schema_version",
    "source_ref",
    "source_head",
    "accepted_tasks",
    "deferred_tasks",
    "created_by",
    "created_at",
}
MIGRATION_ACCEPTED_FIELDS = {
    "task_id",
    "legacy_record_blob",
    "legacy_acceptance_sha",
    "delivery_sha",
}
MIGRATION_DEFERRED_FIELDS = {
    "task_id",
    "legacy_state",
    "archive_ref",
}


class ContractError(RuntimeError):
    """Raised when a Harness V2 contract is invalid."""


def _object_without_duplicates(pairs: Sequence[Tuple[str, Any]]) -> Dict[str, Any]:
    result: Dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ContractError("duplicate JSON key: {}".format(key))
        result[key] = value
    return result


def load_json(path: Path) -> Dict[str, Any]:
    try:
        with path.open(encoding="utf-8") as source:
            value = json.load(source, object_pairs_hook=_object_without_duplicates)
    except (OSError, json.JSONDecodeError) as error:
        raise ContractError("cannot read {}: {}".format(path, error)) from error
    if not isinstance(value, dict):
        raise ContractError("{} must contain one JSON object".format(path))
    return value


def canonical_bytes(value: Any) -> bytes:
    try:
        return json.dumps(
            value,
            ensure_ascii=False,
            allow_nan=False,
            separators=(",", ":"),
            sort_keys=True,
        ).encode("utf-8")
    except (TypeError, ValueError) as error:
        raise ContractError("value is not canonical JSON: {}".format(error)) from error


def canonical_sha256(value: Any) -> str:
    return hashlib.sha256(canonical_bytes(value)).hexdigest()


def plan_content_sha256(plan: Mapping[str, Any]) -> str:
    content = copy.deepcopy(dict(plan))
    content.pop("approval", None)
    return canonical_sha256(content)


def _require_exact(value: Any, fields: Set[str], label: str) -> Mapping[str, Any]:
    if not isinstance(value, dict):
        raise ContractError("{} must be an object".format(label))
    actual = set(value)
    if actual != fields:
        missing = sorted(fields - actual)
        unknown = sorted(actual - fields)
        details = []
        if missing:
            details.append("missing={}".format(",".join(missing)))
        if unknown:
            details.append("unknown={}".format(",".join(unknown)))
        raise ContractError(
            "{} has invalid fields ({})".format(label, "; ".join(details))
        )
    return value


def _require_string(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value.strip() or value != value.strip():
        raise ContractError("{} must be a non-empty trimmed string".format(label))
    if "\x00" in value or "\r" in value:
        raise ContractError("{} contains a forbidden character".format(label))
    return value


def _require_id(value: Any, pattern: re.Pattern, label: str) -> str:
    text = _require_string(value, label)
    if pattern.fullmatch(text) is None:
        raise ContractError("{} has invalid format".format(label))
    return text


def _require_string_list(
    value: Any, label: str, allow_empty: bool = False
) -> List[str]:
    if not isinstance(value, list):
        raise ContractError("{} must be an array".format(label))
    result = [_require_string(item, "{}[]".format(label)) for item in value]
    if not allow_empty and not result:
        raise ContractError("{} must not be empty".format(label))
    if len(result) != len(set(result)):
        raise ContractError("{} contains duplicates".format(label))
    return result


def _require_relative_path(value: str, label: str) -> None:
    path = Path(value)
    if (
        path.is_absolute()
        or ".." in path.parts
        or value == ".git"
        or value.startswith(".git/")
    ):
        raise ContractError("{} is outside the repository".format(label))
    if "\\" in value:
        raise ContractError("{} must use forward slashes".format(label))


def _literal_prefix(pattern: str) -> Tuple[str, ...]:
    result = []
    for part in pattern.split("/"):
        if any(character in part for character in "*?["):
            break
        result.append(part)
    return tuple(result)


def patterns_overlap(left: str, right: str) -> bool:
    if not any(character in left for character in "*?["):
        return fnmatch.fnmatchcase(left, right)
    if not any(character in right for character in "*?["):
        return fnmatch.fnmatchcase(right, left)
    left_prefix = _literal_prefix(left)
    right_prefix = _literal_prefix(right)
    shared = min(len(left_prefix), len(right_prefix))
    return left_prefix[:shared] == right_prefix[:shared]


def _topological_order(graph: Mapping[str, Sequence[str]], label: str) -> List[str]:
    visiting: Set[str] = set()
    visited: Set[str] = set()
    order: List[str] = []

    def visit(node: str) -> None:
        if node in visiting:
            raise ContractError("{} contains a cycle at {}".format(label, node))
        if node in visited:
            return
        visiting.add(node)
        for dependency in graph.get(node, []):
            if dependency not in graph:
                raise ContractError(
                    "{} {} references unknown {}".format(label, node, dependency)
                )
            visit(dependency)
        visiting.remove(node)
        visited.add(node)
        order.append(node)

    for key in sorted(graph):
        visit(key)
    return order


def gate_leaf_ids(gates: Mapping[str, Any], gate_id: str) -> Tuple[str, ...]:
    pending = [gate_id]
    leaves: List[str] = []
    seen: Set[str] = set()
    while pending:
        current = pending.pop()
        gate = gates[current]
        if gate["command"] is not None:
            if current not in seen:
                seen.add(current)
                leaves.append(current)
        else:
            pending.extend(reversed(gate["aggregate"]))
    return tuple(leaves)


def _transitive_dependencies(
    task_id: str, graph: Mapping[str, Sequence[str]]
) -> Set[str]:
    result: Set[str] = set()
    pending = list(graph.get(task_id, []))
    while pending:
        dependency = pending.pop()
        if dependency in result:
            continue
        result.add(dependency)
        pending.extend(graph.get(dependency, []))
    return result


@dataclass(frozen=True)
class ContractSet:
    root: Path
    manifest: Dict[str, Any]
    plans: Dict[str, Dict[str, Any]]
    tasks: Dict[str, Dict[str, Any]]
    gate_policy: Dict[str, Any]
    risk_routing: Dict[str, Any]
    legacy_accepted: FrozenSet[str]

    @property
    def gates(self) -> Mapping[str, Any]:
        return self.gate_policy["gates"]

    @property
    def task_graph(self) -> Mapping[str, Sequence[str]]:
        return {
            task_id: [
                dependency
                for dependency in task["depends_on"]
                if dependency in self.tasks
            ]
            for task_id, task in self.tasks.items()
        }


def task_required_platforms(
    contracts: ContractSet, task_ids: Sequence[str]
) -> Tuple[str, ...]:
    criteria = {
        criterion_id
        for task_id in task_ids
        for criterion_id in contracts.tasks[task_id]["criteria"]
    }
    required = {"linux"}
    for plan in contracts.plans.values():
        for requirement in plan["requirements"]:
            for criterion in requirement["criteria"]:
                if criterion["id"] in criteria:
                    required.update(criterion["evidence"]["platforms"])
    if any(
        RISK_RANK[contracts.tasks[task_id]["risk"]["platform"]] >= RISK_RANK["high"]
        or RISK_RANK[contracts.tasks[task_id]["risk"]["compatibility"]]
        >= RISK_RANK["high"]
        or RISK_RANK[contracts.tasks[task_id]["risk"]["security"]]
        >= RISK_RANK["critical"]
        for task_id in task_ids
    ):
        required.update(EVIDENCE_PLATFORMS)
    return tuple(
        platform for platform in ("linux", "macos", "windows") if platform in required
    )


def _validate_manifest(value: Dict[str, Any]) -> None:
    manifest = _require_exact(value, MANIFEST_FIELDS, "manifest")
    if manifest["schema_version"] != 1 or manifest["harness_version"] != 2:
        raise ContractError("manifest must select Harness V2 schema version 1")
    _require_string(manifest["project"], "manifest.project")
    _require_string(manifest["integration_branch"], "manifest.integration_branch")
    owner = _require_exact(
        manifest["project_owner"], PROJECT_OWNER_FIELDS, "manifest.project_owner"
    )
    _require_string(owner["id"], "manifest.project_owner.id")
    _require_string(owner["name"], "manifest.project_owner.name")
    email = _require_string(owner["email"], "manifest.project_owner.email")
    if "@" not in email:
        raise ContractError("manifest.project_owner.email is invalid")
    refs = _require_exact(
        manifest["ref_namespaces"], REF_NAMESPACE_FIELDS, "manifest.ref_namespaces"
    )
    prefixes = []
    for key in sorted(refs):
        prefix = _require_string(refs[key], "manifest.ref_namespaces.{}".format(key))
        if not prefix.startswith("refs/heads/") or not prefix.endswith("/"):
            raise ContractError(
                "manifest.ref_namespaces.{} must be a branch prefix".format(key)
            )
        prefixes.append(prefix)
    if len(prefixes) != len(set(prefixes)):
        raise ContractError("manifest ref namespaces must be unique")


def _validate_gate_policy(value: Dict[str, Any]) -> None:
    policy = _require_exact(value, GATE_POLICY_FIELDS, "gate policy")
    if policy["schema_version"] != 1:
        raise ContractError("gate policy schema_version must be 1")
    groups = policy["resource_groups"]
    if not isinstance(groups, dict) or not groups:
        raise ContractError("gate policy resource_groups must be a non-empty object")
    for group_id, raw in groups.items():
        _require_id(group_id, GATE_ID, "resource group id")
        group = _require_exact(
            raw, RESOURCE_GROUP_FIELDS, "resource group {}".format(group_id)
        )
        if (
            not isinstance(group["max_parallel"], int)
            or isinstance(group["max_parallel"], bool)
            or group["max_parallel"] < 1
            or group["max_parallel"] > 32
        ):
            raise ContractError(
                "resource group {} max_parallel is invalid".format(group_id)
            )
    gates = policy["gates"]
    if not isinstance(gates, dict) or not gates:
        raise ContractError("gate policy gates must be a non-empty object")
    graph: Dict[str, List[str]] = {}
    commands: Dict[str, str] = {}
    for gate_id, raw in gates.items():
        _require_id(gate_id, GATE_ID, "gate id")
        gate = _require_exact(raw, GATE_FIELDS, "gate {}".format(gate_id))
        command = gate["command"]
        aggregate = gate["aggregate"]
        if (command is None) == (aggregate is None):
            raise ContractError(
                "gate {} must define exactly one command or aggregate".format(gate_id)
            )
        inputs = _require_string_list(
            gate["inputs"], "gate {}.inputs".format(gate_id), allow_empty=True
        )
        for path in inputs:
            _require_relative_path(path, "gate {}.inputs".format(gate_id))
        platforms = _require_string_list(
            gate["platforms"], "gate {}.platforms".format(gate_id)
        )
        if gate["cache"] not in CACHE_MODES:
            raise ContractError("gate {} cache mode is invalid".format(gate_id))
        if command is not None:
            command_value = _require_exact(
                command, COMMAND_FIELDS, "gate {}.command".format(gate_id)
            )
            argv = _require_string_list(
                command_value["argv"], "gate {}.command.argv".format(gate_id)
            )
            timeout = command_value["timeout_seconds"]
            if (
                not isinstance(timeout, int)
                or isinstance(timeout, bool)
                or timeout < 1
                or timeout > 86_400
            ):
                raise ContractError("gate {} timeout is invalid".format(gate_id))
            environment = command_value["environment"]
            if not isinstance(environment, dict):
                raise ContractError(
                    "gate {} environment must be an object".format(gate_id)
                )
            for name, item in environment.items():
                if re.fullmatch(r"[A-Z][A-Z0-9_]*", name) is None:
                    raise ContractError(
                        "gate {} environment name is invalid".format(gate_id)
                    )
                _require_string(item, "gate {} environment value".format(gate_id))
            group = _require_string(
                gate["resource_group"], "gate {}.resource_group".format(gate_id)
            )
            if group not in groups:
                raise ContractError(
                    "gate {} references unknown resource group {}".format(
                        gate_id, group
                    )
                )
            digest = canonical_sha256({"argv": argv, "environment": environment})
            if digest in commands:
                raise ContractError(
                    "leaf gates {} and {} duplicate one command".format(
                        commands[digest], gate_id
                    )
                )
            commands[digest] = gate_id
            graph[gate_id] = []
        else:
            dependencies = _require_string_list(
                aggregate, "gate {}.aggregate".format(gate_id)
            )
            if gate["resource_group"] is not None:
                raise ContractError(
                    "aggregate gate {} must not own a resource group".format(gate_id)
                )
            graph[gate_id] = dependencies
    _topological_order(graph, "gate graph")


def _validate_risk_routing(value: Dict[str, Any], gates: Mapping[str, Any]) -> None:
    routing = _require_exact(value, RISK_ROUTING_FIELDS, "risk routing")
    if routing["schema_version"] != 1:
        raise ContractError("risk routing schema_version must be 1")
    rules = routing["path_rules"]
    if not isinstance(rules, list):
        raise ContractError("risk routing path_rules must be an array")
    for index, raw in enumerate(rules):
        rule = _require_exact(raw, PATH_RULE_FIELDS, "path rule {}".format(index))
        paths = _require_string_list(rule["paths"], "path rule {}.paths".format(index))
        for path in paths:
            _require_relative_path(path, "path rule {}.paths".format(index))
        minimum = rule["minimum_risk"]
        if not isinstance(minimum, dict) or not minimum:
            raise ContractError(
                "path rule {} minimum_risk must be a non-empty object".format(index)
            )
        for dimension, level in minimum.items():
            if dimension not in RISK_DIMENSIONS or level not in RISK_RANK:
                raise ContractError(
                    "path rule {} has invalid minimum risk".format(index)
                )
        for gate_id in _require_string_list(
            rule["required_gates"], "path rule {}.required_gates".format(index)
        ):
            if gate_id not in gates:
                raise ContractError(
                    "path rule {} references unknown gate {}".format(index, gate_id)
                )
    minimums = routing["phase_minimums"]
    if not isinstance(minimums, dict):
        raise ContractError("risk routing phase_minimums must be an object")
    for phase, gate_ids in minimums.items():
        _require_string(phase, "phase name")
        for gate_id in _require_string_list(
            gate_ids, "phase {} gates".format(phase), allow_empty=True
        ):
            if gate_id not in gates:
                raise ContractError(
                    "phase {} references unknown gate {}".format(phase, gate_id)
                )


def _validate_plan(
    value: Dict[str, Any],
    manifest: Mapping[str, Any],
    gates: Mapping[str, Any],
) -> None:
    plan = _require_exact(value, PLAN_FIELDS, "plan")
    if plan["schema_version"] != 1:
        raise ContractError("plan schema_version must be 1")
    plan_id = _require_id(plan["id"], PLAN_ID, "plan.id")
    _require_string(plan["title"], "{}.title".format(plan_id))
    if plan["status"] not in PLAN_STATUSES:
        raise ContractError("{} status is invalid".format(plan_id))
    source = _require_exact(
        plan["source"], PLAN_SOURCE_FIELDS, "{}.source".format(plan_id)
    )
    _require_string(source["kind"], "{}.source.kind".format(plan_id))
    source_path = _require_string(source["path"], "{}.source.path".format(plan_id))
    _require_relative_path(source_path, "{}.source.path".format(plan_id))
    requirements = plan["requirements"]
    if not isinstance(requirements, list) or not requirements:
        raise ContractError("{} requirements must be a non-empty array".format(plan_id))
    requirement_ids: Set[str] = set()
    criterion_ids: Set[str] = set()
    for req_index, raw_requirement in enumerate(requirements):
        requirement = _require_exact(
            raw_requirement,
            REQUIREMENT_FIELDS,
            "{}.requirements[{}]".format(plan_id, req_index),
        )
        requirement_id = _require_id(
            requirement["id"],
            REQUIREMENT_ID,
            "{}.requirements[{}].id".format(plan_id, req_index),
        )
        if requirement_id in requirement_ids:
            raise ContractError("{} duplicates {}".format(plan_id, requirement_id))
        requirement_ids.add(requirement_id)
        _require_string(
            requirement["statement"], "{}.{}.statement".format(plan_id, requirement_id)
        )
        criteria = requirement["criteria"]
        if not isinstance(criteria, list) or not criteria:
            raise ContractError(
                "{}.{} criteria must be non-empty".format(plan_id, requirement_id)
            )
        for criterion_index, raw_criterion in enumerate(criteria):
            criterion = _require_exact(
                raw_criterion,
                CRITERION_FIELDS,
                "{}.{}.criteria[{}]".format(plan_id, requirement_id, criterion_index),
            )
            criterion_id = _require_id(
                criterion["id"],
                CRITERION_ID,
                "{} criterion id".format(plan_id),
            )
            if criterion_id in criterion_ids:
                raise ContractError("{} duplicates {}".format(plan_id, criterion_id))
            criterion_ids.add(criterion_id)
            _require_string(
                criterion["statement"], "{}.{}.statement".format(plan_id, criterion_id)
            )
            _require_string_list(
                criterion["negative_definitions"],
                "{}.{}.negative_definitions".format(plan_id, criterion_id),
            )
            evidence = _require_exact(
                criterion["evidence"],
                EVIDENCE_FIELDS,
                "{}.{}.evidence".format(plan_id, criterion_id),
            )
            for gate_id in _require_string_list(
                evidence["gates"], "{}.{}.evidence.gates".format(plan_id, criterion_id)
            ):
                if gate_id not in gates:
                    raise ContractError(
                        "{} references unknown gate {}".format(criterion_id, gate_id)
                    )
            _require_string_list(
                evidence["scenarios"],
                "{}.{}.evidence.scenarios".format(plan_id, criterion_id),
            )
            _require_string(
                evidence["topology"],
                "{}.{}.evidence.topology".format(plan_id, criterion_id),
            )
            platforms = _require_string_list(
                evidence["platforms"],
                "{}.{}.evidence.platforms".format(plan_id, criterion_id),
                allow_empty=True,
            )
            unknown_platforms = sorted(set(platforms) - EVIDENCE_PLATFORMS)
            if unknown_platforms:
                raise ContractError(
                    "{} evidence platforms are invalid: {}".format(
                        criterion_id, ", ".join(unknown_platforms)
                    )
                )
            for gate_id in evidence["gates"]:
                leaves = gate_leaf_ids(gates, gate_id)
                for platform in platforms:
                    unsupported = sorted(
                        leaf
                        for leaf in leaves
                        if platform not in gates[leaf]["platforms"]
                    )
                    if unsupported:
                        raise ContractError(
                            "{} Gate {} cannot produce {} evidence: {}".format(
                                criterion_id,
                                gate_id,
                                platform,
                                ", ".join(unsupported),
                            )
                        )
            _require_string_list(
                evidence["roles"],
                "{}.{}.evidence.roles".format(plan_id, criterion_id),
                allow_empty=True,
            )
            if not isinstance(evidence["allow_skipped"], bool):
                raise ContractError(
                    "{}.{}.evidence.allow_skipped must be boolean".format(
                        plan_id, criterion_id
                    )
                )
            if evidence["allow_skipped"]:
                raise ContractError(
                    "{}.{} cannot allow skipped evidence".format(plan_id, criterion_id)
                )
        for task_id in _require_string_list(
            requirement["implementation_tasks"],
            "{}.{}.implementation_tasks".format(plan_id, requirement_id),
        ):
            _require_id(task_id, TASK_ID, "implementation task")
        _require_id(requirement["acceptance_owner"], TASK_ID, "acceptance owner")
    approval = plan["approval"]
    if plan["status"] == "draft":
        if approval is not None:
            raise ContractError("{} draft approval must be null".format(plan_id))
    else:
        approved = _require_exact(
            approval, APPROVAL_FIELDS, "{}.approval".format(plan_id)
        )
        owner_id = manifest["project_owner"]["id"]
        if approved["approved_by"] != owner_id:
            raise ContractError(
                "{} approval is not owned by configured project owner".format(plan_id)
            )
        _require_string(
            approved["approved_at"], "{}.approval.approved_at".format(plan_id)
        )
        digest = _require_string(
            approved["content_sha256"], "{}.approval.content_sha256".format(plan_id)
        )
        if SHA256.fullmatch(digest) is None or digest != plan_content_sha256(plan):
            raise ContractError(
                "{} approval digest does not match content".format(plan_id)
            )


def _validate_task(
    value: Dict[str, Any],
    gates: Mapping[str, Any],
    routing: Mapping[str, Any],
) -> None:
    task = _require_exact(value, TASK_FIELDS, "task")
    if task["schema_version"] != 1:
        raise ContractError("task schema_version must be 1")
    task_id = _require_id(task["id"], TASK_ID, "task.id")
    _require_string(task["title"], "{}.title".format(task_id))
    _require_id(task["plan"], PLAN_ID, "{}.plan".format(task_id))
    criteria = _require_string_list(task["criteria"], "{}.criteria".format(task_id))
    for criterion_id in criteria:
        _require_id(criterion_id, CRITERION_ID, "{} criterion".format(task_id))
    dependencies = _require_string_list(
        task["depends_on"], "{}.depends_on".format(task_id), allow_empty=True
    )
    for dependency in dependencies:
        _require_id(dependency, TASK_ID, "{} dependency".format(task_id))
        if dependency == task_id:
            raise ContractError("{} depends on itself".format(task_id))
    owned_paths = _require_string_list(
        task["owned_paths"], "{}.owned_paths".format(task_id)
    )
    for path in owned_paths:
        _require_relative_path(path, "{}.owned_paths".format(task_id))
    task_type = task["type"]
    if task_type not in TASK_TYPES:
        raise ContractError("{} type is invalid".format(task_id))
    if task_type == "acceptance" and owned_paths != [
        ".agents/tasks/{}.json".format(task_id)
    ]:
        raise ContractError(
            "{} acceptance task must own only its TaskSpec".format(task_id)
        )
    _require_string(task["workstream"], "{}.workstream".format(task_id))
    risk = task["risk"]
    if not isinstance(risk, dict) or set(risk) != set(RISK_DIMENSIONS):
        raise ContractError("{} risk dimensions are incomplete".format(task_id))
    for dimension in RISK_DIMENSIONS:
        if risk[dimension] not in RISK_RANK:
            raise ContractError("{} risk {} is invalid".format(task_id, dimension))
    tdd = _require_exact(task["tdd"], TDD_FIELDS, "{}.tdd".format(task_id))
    if tdd["mode"] not in TDD_MODE_BY_TYPE[task_type]:
        raise ContractError("{} TDD mode does not match task type".format(task_id))
    proof_paths = _require_string_list(
        tdd["proof_paths"], "{}.tdd.proof_paths".format(task_id), allow_empty=True
    )
    oracle_paths = _require_string_list(
        tdd["oracle_paths"], "{}.tdd.oracle_paths".format(task_id), allow_empty=True
    )
    fingerprints = _require_string_list(
        tdd["failure_fingerprints"],
        "{}.tdd.failure_fingerprints".format(task_id),
        allow_empty=True,
    )
    for path in proof_paths + oracle_paths:
        _require_relative_path(path, "{} TDD path".format(task_id))
        if not any(patterns_overlap(path, owned) for owned in owned_paths):
            raise ContractError(
                "{} TDD path is outside owned paths: {}".format(task_id, path)
            )
    requires_red = tdd["mode"] in {
        "red_green",
        "regression",
        "mutation",
        "adversarial",
    }
    if requires_red:
        gate_id = _require_string(tdd["gate"], "{}.tdd.gate".format(task_id))
        if gate_id not in gates:
            raise ContractError("{} TDD gate is unknown".format(task_id))
        if len(gate_leaf_ids(gates, gate_id)) != 1:
            raise ContractError(
                "{} TDD gate must resolve to exactly one leaf".format(task_id)
            )
        if not proof_paths or not fingerprints:
            raise ContractError("{} Red contract is incomplete".format(task_id))
    elif tdd["gate"] is not None:
        gate_id = _require_string(tdd["gate"], "{}.tdd.gate".format(task_id))
        if gate_id not in gates:
            raise ContractError("{} TDD gate is unknown".format(task_id))
    for fingerprint in fingerprints:
        if len(fingerprint) < 16 or len(fingerprint) > 256 or "\n" in fingerprint:
            raise ContractError("{} failure fingerprint is invalid".format(task_id))
    delivery = _require_exact(
        task["delivery"], DELIVERY_FIELDS, "{}.delivery".format(task_id)
    )
    if (
        re.fullmatch(
            r"(feat|fix|docs|style|refactor|perf|test|build|ci|chore|revert)",
            _require_string(delivery["commit_type"], "{} commit type".format(task_id)),
        )
        is None
    ):
        raise ContractError("{} commit type is invalid".format(task_id))
    if (
        re.fullmatch(
            r"[a-z][a-z0-9-]*",
            _require_string(delivery["scope"], "{} scope".format(task_id)),
        )
        is None
    ):
        raise ContractError("{} delivery scope is invalid".format(task_id))
    summary = _require_string(delivery["summary"], "{} summary".format(task_id))
    if len(summary) > 60 or summary.endswith("."):
        raise ContractError("{} delivery summary is invalid".format(task_id))
    architecture = _require_exact(
        delivery["architecture_change"],
        ARCHITECTURE_CHANGE_FIELDS,
        "{} architecture change".format(task_id),
    )
    if architecture["mode"] not in ARCHITECTURE_MODES:
        raise ContractError("{} architecture mode is invalid".format(task_id))
    modules = _require_string_list(
        architecture["modules"],
        "{} architecture modules".format(task_id),
        allow_empty=True,
    )
    supersedes = _require_exact(
        architecture["supersedes"],
        SUPERSEDES_FIELDS,
        "{} architecture supersedes".format(task_id),
    )
    superseded_paths = _require_string_list(
        supersedes["paths"],
        "{} superseded paths".format(task_id),
        allow_empty=True,
    )
    superseded_targets = _require_string_list(
        supersedes["targets"],
        "{} superseded targets".format(task_id),
        allow_empty=True,
    )
    for path in superseded_paths:
        _require_relative_path(path, "{} superseded path".format(task_id))
        if not any(patterns_overlap(path, owned) for owned in owned_paths):
            raise ContractError(
                "{} superseded path is outside ownership: {}".format(task_id, path)
            )
    for target in superseded_targets:
        if re.fullmatch(r"xnn_transfer_[a-z0-9_]+", target) is None:
            raise ContractError("{} superseded target is invalid".format(task_id))
    symbols = supersedes["symbols"]
    if not isinstance(symbols, list):
        raise ContractError("{} superseded symbols must be an array".format(task_id))
    symbol_keys = set()
    for index, raw_symbol in enumerate(symbols):
        symbol = _require_exact(
            raw_symbol,
            SYMBOL_FIELDS,
            "{} superseded symbol {}".format(task_id, index),
        )
        path = _require_string(symbol["path"], "{} symbol path".format(task_id))
        name = _require_string(symbol["name"], "{} symbol name".format(task_id))
        _require_relative_path(path, "{} symbol path".format(task_id))
        if not any(patterns_overlap(path, owned) for owned in owned_paths):
            raise ContractError(
                "{} superseded symbol is outside ownership".format(task_id)
            )
        key = (path, name)
        if key in symbol_keys:
            raise ContractError(
                "{} superseded symbols contain duplicates".format(task_id)
            )
        symbol_keys.add(key)
    leases = architecture["temporary_leases"]
    if not isinstance(leases, list):
        raise ContractError("{} temporary leases must be an array".format(task_id))
    lease_ids = set()
    for index, raw_lease in enumerate(leases):
        lease = _require_exact(
            raw_lease,
            LEASE_FIELDS,
            "{} temporary lease {}".format(task_id, index),
        )
        lease_id = _require_string(lease["id"], "{} lease id".format(task_id))
        if re.fullmatch(r"[a-z0-9][a-z0-9._-]*", lease_id) is None:
            raise ContractError("{} lease id is invalid".format(task_id))
        if lease_id in lease_ids:
            raise ContractError("{} temporary lease IDs are duplicated".format(task_id))
        lease_ids.add(lease_id)
        lease_path = _require_string(lease["path"], "{} lease path".format(task_id))
        _require_relative_path(lease_path, "{} lease path".format(task_id))
        if not any(patterns_overlap(lease_path, owned) for owned in owned_paths):
            raise ContractError("{} lease path is outside ownership".format(task_id))
        _require_id(
            lease["remove_by_task"], TASK_ID, "{} lease removal task".format(task_id)
        )
        if (
            len(_require_string(lease["reason"], "{} lease reason".format(task_id)))
            < 20
        ):
            raise ContractError("{} lease reason is too short".format(task_id))
    retires = _require_string_list(
        architecture["retires_leases"],
        "{} retired leases".format(task_id),
        allow_empty=True,
    )
    for lease_id in retires:
        if re.fullmatch(r"[a-z0-9][a-z0-9._-]*", lease_id) is None:
            raise ContractError("{} retired lease ID is invalid".format(task_id))
    has_supersession = bool(superseded_paths or superseded_targets or symbols)
    mode = architecture["mode"]
    if mode == "none" and (modules or has_supersession or leases or retires):
        raise ContractError(
            "{} mode none declares architecture changes".format(task_id)
        )
    if mode in {"add", "replace"} and not modules:
        raise ContractError("{} mode {} requires modules".format(task_id, mode))
    if mode == "remove" and not (modules or has_supersession or retires):
        raise ContractError("{} remove mode has no removal claim".format(task_id))
    if mode == "refactor" and not (modules or has_supersession or leases):
        raise ContractError("{} refactor mode has no affected boundary".format(task_id))
    for index, rule in enumerate(routing["path_rules"]):
        if not any(
            patterns_overlap(owned, routed)
            for owned in owned_paths
            for routed in rule["paths"]
        ):
            continue
        for dimension, minimum in rule["minimum_risk"].items():
            if RISK_RANK[risk[dimension]] < RISK_RANK[minimum]:
                raise ContractError(
                    "{} lowers routed {} risk below {}".format(
                        task_id, dimension, minimum
                    )
                )


def _load_migration_snapshot(path: Path) -> FrozenSet[str]:
    if not path.is_file():
        return frozenset()
    snapshot = _require_exact(
        load_json(path), MIGRATION_FIELDS, "V1 migration snapshot"
    )
    if snapshot["schema_version"] != 1:
        raise ContractError("V1 migration snapshot schema_version must be 1")
    _require_string(snapshot["source_ref"], "migration source_ref")
    source_head = _require_string(snapshot["source_head"], "migration source_head")
    if len(source_head) != 40:
        raise ContractError("migration source_head is invalid")
    _require_string(snapshot["created_by"], "migration created_by")
    _require_string(snapshot["created_at"], "migration created_at")
    accepted = snapshot["accepted_tasks"]
    deferred = snapshot["deferred_tasks"]
    if not isinstance(accepted, list) or not isinstance(deferred, list):
        raise ContractError("migration task lists must be arrays")
    accepted_ids: Set[str] = set()
    deferred_ids: Set[str] = set()
    for index, raw in enumerate(accepted):
        item = _require_exact(
            raw,
            MIGRATION_ACCEPTED_FIELDS,
            "migration accepted task {}".format(index),
        )
        task_id = _require_id(item["task_id"], TASK_ID, "migration task id")
        if task_id in accepted_ids:
            raise ContractError("migration accepted tasks contain duplicates")
        accepted_ids.add(task_id)
        for field in (
            "legacy_record_blob",
            "legacy_acceptance_sha",
            "delivery_sha",
        ):
            value = _require_string(
                item[field], "migration {} {}".format(task_id, field)
            )
            if len(value) != 40:
                raise ContractError("migration {} {} is invalid".format(task_id, field))
    for index, raw in enumerate(deferred):
        item = _require_exact(
            raw,
            MIGRATION_DEFERRED_FIELDS,
            "migration deferred task {}".format(index),
        )
        task_id = _require_id(item["task_id"], TASK_ID, "migration task id")
        if task_id in deferred_ids:
            raise ContractError("migration deferred tasks contain duplicates")
        deferred_ids.add(task_id)
        _require_string(item["legacy_state"], "migration legacy state")
        archive_ref = _require_string(item["archive_ref"], "migration archive ref")
        if not archive_ref.startswith("refs/heads/archive/"):
            raise ContractError("migration archive ref is invalid")
    overlap = sorted(accepted_ids & deferred_ids)
    if overlap:
        raise ContractError(
            "migration tasks are both accepted and deferred: {}".format(
                ", ".join(overlap)
            )
        )
    return frozenset(accepted_ids)


def _validate_cross_contracts(contracts: ContractSet) -> None:
    task_graph = contracts.task_graph
    _topological_order(task_graph, "task graph")
    criterion_to_plan: Dict[str, str] = {}
    allowed_task_criteria: Set[Tuple[str, str]] = set()
    for plan_id, plan in contracts.plans.items():
        for requirement in plan["requirements"]:
            requirement_criteria = {
                criterion["id"] for criterion in requirement["criteria"]
            }
            for criterion in requirement["criteria"]:
                criterion_id = criterion["id"]
                if criterion_id in criterion_to_plan:
                    raise ContractError(
                        "criterion {} appears in multiple plans".format(criterion_id)
                    )
                criterion_to_plan[criterion_id] = plan_id
            covered: Set[str] = set()
            for task_id in requirement["implementation_tasks"]:
                if task_id not in contracts.tasks:
                    raise ContractError(
                        "{} references unknown implementation task {}".format(
                            requirement["id"], task_id
                        )
                    )
                if contracts.tasks[task_id]["type"] == "acceptance":
                    raise ContractError(
                        "{} implementation task {} cannot have acceptance "
                        "type".format(requirement["id"], task_id)
                    )
                task_criteria = (
                    set(contracts.tasks[task_id]["criteria"]) & requirement_criteria
                )
                if not task_criteria:
                    raise ContractError(
                        "{} implementation task {} maps no requirement "
                        "criterion".format(requirement["id"], task_id)
                    )
                covered.update(task_criteria)
                allowed_task_criteria.update(
                    (task_id, criterion_id) for criterion_id in task_criteria
                )
            missing_implementation = sorted(requirement_criteria - covered)
            if missing_implementation:
                raise ContractError(
                    "{} criteria are not covered by implementation tasks: "
                    "{}".format(
                        requirement["id"],
                        ", ".join(missing_implementation),
                    )
                )
            acceptance = requirement["acceptance_owner"]
            if acceptance not in contracts.tasks:
                raise ContractError(
                    "{} references unknown acceptance owner {}".format(
                        requirement["id"], acceptance
                    )
                )
            if contracts.tasks[acceptance]["type"] != "acceptance":
                raise ContractError(
                    "{} acceptance owner must have acceptance type".format(
                        requirement["id"]
                    )
                )
            missing_acceptance = sorted(
                requirement_criteria - set(contracts.tasks[acceptance]["criteria"])
            )
            if missing_acceptance:
                raise ContractError(
                    "{} acceptance owner does not map criteria: {}".format(
                        requirement["id"], ", ".join(missing_acceptance)
                    )
                )
            allowed_task_criteria.update(
                (acceptance, criterion_id) for criterion_id in requirement_criteria
            )
            dependencies = _transitive_dependencies(acceptance, task_graph)
            missing = sorted(set(requirement["implementation_tasks"]) - dependencies)
            if missing:
                raise ContractError(
                    "{} acceptance owner does not depend on {}".format(
                        requirement["id"], ", ".join(missing)
                    )
                )
    mapped_criteria: Set[str] = set()
    module_document = load_json(
        contracts.root / ".agents" / "architecture" / "modules.json"
    )
    raw_modules = module_document.get("modules")
    if not isinstance(raw_modules, list):
        raise ContractError("architecture module inventory is invalid")
    known_modules = {
        item["id"]
        for item in raw_modules
        if isinstance(item, dict) and isinstance(item.get("id"), str)
    }
    leases: Dict[str, Tuple[str, Mapping[str, Any]]] = {}
    retired_by: Dict[str, str] = {}
    for task_id, task in contracts.tasks.items():
        unknown_dependencies = sorted(
            set(task["depends_on"])
            - set(contracts.tasks)
            - set(contracts.legacy_accepted)
        )
        if unknown_dependencies:
            raise ContractError(
                "{} references unknown dependencies: {}".format(
                    task_id, ", ".join(unknown_dependencies)
                )
            )
        plan_id = task["plan"]
        if plan_id not in contracts.plans:
            raise ContractError(
                "{} references unknown plan {}".format(task_id, plan_id)
            )
        if contracts.plans[plan_id]["status"] != "approved":
            raise ContractError("{} references an unapproved plan".format(task_id))
        for criterion_id in task["criteria"]:
            if criterion_to_plan.get(criterion_id) != plan_id:
                raise ContractError(
                    "{} references unknown or foreign criterion {}".format(
                        task_id, criterion_id
                    )
                )
            if (task_id, criterion_id) not in allowed_task_criteria:
                raise ContractError(
                    "{} is not assigned to criterion {} by its requirement".format(
                        task_id, criterion_id
                    )
                )
            mapped_criteria.add(criterion_id)
        architecture = task["delivery"]["architecture_change"]
        unknown_modules = sorted(set(architecture["modules"]) - known_modules)
        if unknown_modules:
            raise ContractError(
                "{} references unknown architecture modules: {}".format(
                    task_id, ", ".join(unknown_modules)
                )
            )
        for lease in architecture["temporary_leases"]:
            lease_id = lease["id"]
            if lease_id in leases:
                raise ContractError(
                    "temporary lease {} is declared more than once".format(lease_id)
                )
            removal = lease["remove_by_task"]
            if removal not in contracts.tasks:
                raise ContractError(
                    "{} lease {} has unknown removal task {}".format(
                        task_id, lease_id, removal
                    )
                )
            if removal == task_id:
                raise ContractError(
                    "{} lease {} cannot remove itself".format(task_id, lease_id)
                )
            leases[lease_id] = (task_id, lease)
        for lease_id in architecture["retires_leases"]:
            if lease_id in retired_by:
                raise ContractError(
                    "temporary lease {} has multiple removal tasks".format(lease_id)
                )
            retired_by[lease_id] = task_id
    for lease_id, removal_task in retired_by.items():
        if lease_id not in leases:
            raise ContractError(
                "{} retires unknown temporary lease {}".format(removal_task, lease_id)
            )
        expected = leases[lease_id][1]["remove_by_task"]
        if removal_task != expected:
            raise ContractError(
                "{} must retire lease {}, not {}".format(
                    expected, lease_id, removal_task
                )
            )
    missing_criteria = sorted(set(criterion_to_plan) - mapped_criteria)
    if missing_criteria:
        raise ContractError(
            "criteria have no TaskSpec mapping: {}".format(", ".join(missing_criteria))
        )
    criterion_documents = {
        criterion["id"]: criterion
        for plan in contracts.plans.values()
        for requirement in plan["requirements"]
        for criterion in requirement["criteria"]
    }
    for task_id, task in contracts.tasks.items():
        platforms = task_required_platforms(contracts, [task_id])
        for criterion_id in task["criteria"]:
            criterion = criterion_documents[criterion_id]
            leaves = {
                leaf
                for gate_id in criterion["evidence"]["gates"]
                for leaf in gate_leaf_ids(contracts.gates, gate_id)
            }
            for platform in platforms:
                unsupported = sorted(
                    leaf
                    for leaf in leaves
                    if platform not in contracts.gates[leaf]["platforms"]
                )
                if unsupported:
                    raise ContractError(
                        "{} cannot satisfy criterion {} on {}: {}".format(
                            task_id,
                            criterion_id,
                            platform,
                            ", ".join(unsupported),
                        )
                    )
    for left_id, left in contracts.tasks.items():
        for right_id, right in contracts.tasks.items():
            if left_id >= right_id:
                continue
            for left_path in left["owned_paths"]:
                for right_path in right["owned_paths"]:
                    if patterns_overlap(left_path, right_path):
                        # Static overlap is legal only when dependencies serialize it.
                        left_deps = _transitive_dependencies(left_id, task_graph)
                        right_deps = _transitive_dependencies(right_id, task_graph)
                        if right_id not in left_deps and left_id not in right_deps:
                            raise ContractError(
                                "{} and {} have unordered overlapping paths: {} <-> {}".format(
                                    left_id, right_id, left_path, right_path
                                )
                            )


def load_contracts(root: Path) -> ContractSet:
    root = root.resolve()
    agents = root / ".agents"
    manifest = load_json(agents / "manifest.json")
    _validate_manifest(manifest)
    gate_policy = load_json(agents / "gates.json")
    _validate_gate_policy(gate_policy)
    risk_routing = load_json(agents / "risk-routing.json")
    _validate_risk_routing(risk_routing, gate_policy["gates"])
    legacy_accepted = _load_migration_snapshot(agents / "migration-v1.json")
    plans: Dict[str, Dict[str, Any]] = {}
    for path in sorted((agents / "plans").glob("DP-*.json")):
        plan = load_json(path)
        _validate_plan(plan, manifest, gate_policy["gates"])
        plan_id = plan["id"]
        if plan_id in plans:
            raise ContractError("duplicate plan id {}".format(plan_id))
        plans[plan_id] = plan
    tasks: Dict[str, Dict[str, Any]] = {}
    for path in sorted((agents / "tasks").glob("XT-*.json")):
        task = load_json(path)
        _validate_task(task, gate_policy["gates"], risk_routing)
        task_id = task["id"]
        if task_id in tasks:
            raise ContractError("duplicate task id {}".format(task_id))
        tasks[task_id] = task
    contracts = ContractSet(
        root=root,
        manifest=manifest,
        plans=plans,
        tasks=tasks,
        gate_policy=gate_policy,
        risk_routing=risk_routing,
        legacy_accepted=legacy_accepted,
    )
    _validate_cross_contracts(contracts)
    return contracts


def approve_plan(root: Path, plan_path: Path, now: str) -> str:
    root = root.resolve()
    manifest = load_json(root / ".agents" / "manifest.json")
    _validate_manifest(manifest)
    plan = load_json(plan_path)
    if plan.get("status") != "draft" or plan.get("approval") is not None:
        raise ContractError("only an unapproved draft plan can be approved")
    email_result = subprocess.run(
        ["git", "-C", str(root), "config", "--get", "user.email"],
        check=False,
        capture_output=True,
        text=True,
    )
    email = email_result.stdout.strip()
    owner = manifest["project_owner"]
    if email_result.returncode != 0 or email != owner["email"]:
        raise ContractError("current Git identity is not the configured project owner")
    plan["status"] = "approved"
    plan["approval"] = {
        "approved_by": owner["id"],
        "approved_at": _require_string(now, "approval time"),
        "content_sha256": "",
    }
    plan["approval"]["content_sha256"] = plan_content_sha256(plan)
    _validate_plan(plan, manifest, load_json(root / ".agents" / "gates.json")["gates"])
    with tempfile.NamedTemporaryFile(
        mode="w",
        encoding="utf-8",
        dir=str(plan_path.parent),
        prefix=".plan-",
        suffix=".json",
        delete=False,
    ) as output:
        json.dump(plan, output, ensure_ascii=False, indent=2, sort_keys=False)
        output.write("\n")
        temporary = Path(output.name)
    temporary.replace(plan_path)
    return plan["approval"]["content_sha256"]
