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
from typing import Any, Dict, List, Mapping, Sequence, Set, Tuple


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

MANIFEST_FIELDS = {
    "schema_version",
    "harness_version",
    "project",
    "integration_branch",
    "project_owner",
    "ref_namespaces",
}
PROJECT_OWNER_FIELDS = {"id", "name", "email"}
REF_NAMESPACE_FIELDS = {"state", "submit", "queue", "attest", "archive"}
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
ARCHITECTURE_CHANGE_FIELDS = {"mode", "modules"}
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


def _require_exact(
    value: Any, fields: Set[str], label: str
) -> Mapping[str, Any]:
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
        raise ContractError("{} has invalid fields ({})".format(label, "; ".join(details)))
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
    if path.is_absolute() or ".." in path.parts or value.startswith(".git"):
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


def _topological_order(
    graph: Mapping[str, Sequence[str]], label: str
) -> List[str]:
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

    @property
    def gates(self) -> Mapping[str, Any]:
        return self.gate_policy["gates"]

    @property
    def task_graph(self) -> Mapping[str, Sequence[str]]:
        return {
            task_id: list(task["depends_on"])
            for task_id, task in self.tasks.items()
        }


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
        prefix = _require_string(
            refs[key], "manifest.ref_namespaces.{}".format(key)
        )
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
                raise ContractError("gate {} environment must be an object".format(gate_id))
            for name, item in environment.items():
                if re.fullmatch(r"[A-Z][A-Z0-9_]*", name) is None:
                    raise ContractError("gate {} environment name is invalid".format(gate_id))
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


def _validate_risk_routing(
    value: Dict[str, Any], gates: Mapping[str, Any]
) -> None:
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
    source = _require_exact(plan["source"], PLAN_SOURCE_FIELDS, "{}.source".format(plan_id))
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
                "{}.{}.criteria[{}]".format(
                    plan_id, requirement_id, criterion_index
                ),
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
            _require_string_list(
                evidence["platforms"],
                "{}.{}.evidence.platforms".format(plan_id, criterion_id),
                allow_empty=True,
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
        for task_id in _require_string_list(
            requirement["implementation_tasks"],
            "{}.{}.implementation_tasks".format(plan_id, requirement_id),
        ):
            _require_id(task_id, TASK_ID, "implementation task")
        _require_id(
            requirement["acceptance_owner"], TASK_ID, "acceptance owner"
        )
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
        _require_string(approved["approved_at"], "{}.approval.approved_at".format(plan_id))
        digest = _require_string(
            approved["content_sha256"], "{}.approval.content_sha256".format(plan_id)
        )
        if SHA256.fullmatch(digest) is None or digest != plan_content_sha256(plan):
            raise ContractError("{} approval digest does not match content".format(plan_id))


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
    if re.fullmatch(
        r"(feat|fix|docs|style|refactor|perf|test|build|ci|chore|revert)",
        _require_string(delivery["commit_type"], "{} commit type".format(task_id)),
    ) is None:
        raise ContractError("{} commit type is invalid".format(task_id))
    if re.fullmatch(
        r"[a-z][a-z0-9-]*",
        _require_string(delivery["scope"], "{} scope".format(task_id)),
    ) is None:
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
    _require_string_list(
        architecture["modules"],
        "{} architecture modules".format(task_id),
        allow_empty=True,
    )
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


def _validate_cross_contracts(contracts: ContractSet) -> None:
    task_graph = contracts.task_graph
    _topological_order(task_graph, "task graph")
    criterion_to_plan: Dict[str, str] = {}
    requirement_owners: Dict[str, Tuple[List[str], str]] = {}
    for plan_id, plan in contracts.plans.items():
        for requirement in plan["requirements"]:
            for criterion in requirement["criteria"]:
                criterion_id = criterion["id"]
                if criterion_id in criterion_to_plan:
                    raise ContractError(
                        "criterion {} appears in multiple plans".format(criterion_id)
                    )
                criterion_to_plan[criterion_id] = plan_id
            requirement_owners[requirement["id"]] = (
                list(requirement["implementation_tasks"]),
                requirement["acceptance_owner"],
            )
            for task_id in requirement["implementation_tasks"]:
                if task_id not in contracts.tasks:
                    raise ContractError(
                        "{} references unknown implementation task {}".format(
                            requirement["id"], task_id
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
            dependencies = _transitive_dependencies(acceptance, task_graph)
            missing = sorted(set(requirement["implementation_tasks"]) - dependencies)
            if missing:
                raise ContractError(
                    "{} acceptance owner does not depend on {}".format(
                        requirement["id"], ", ".join(missing)
                    )
                )
    mapped_criteria: Set[str] = set()
    for task_id, task in contracts.tasks.items():
        plan_id = task["plan"]
        if plan_id not in contracts.plans:
            raise ContractError("{} references unknown plan {}".format(task_id, plan_id))
        if contracts.plans[plan_id]["status"] != "approved":
            raise ContractError("{} references an unapproved plan".format(task_id))
        for criterion_id in task["criteria"]:
            if criterion_to_plan.get(criterion_id) != plan_id:
                raise ContractError(
                    "{} references unknown or foreign criterion {}".format(
                        task_id, criterion_id
                    )
                )
            mapped_criteria.add(criterion_id)
    missing_criteria = sorted(set(criterion_to_plan) - mapped_criteria)
    if missing_criteria:
        raise ContractError(
            "criteria have no TaskSpec mapping: {}".format(", ".join(missing_criteria))
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
    plans: Dict[str, Dict[str, Any]] = {}
    for path in sorted((agents / "plans").glob("DP-*.json")):
        plan = load_json(path)
        _validate_plan(plan, manifest, gate_policy["gates"])
        plan_id = plan["id"]
        if plan_id in plans:
            raise ContractError("duplicate plan id {}".format(plan_id))
        plans[plan_id] = plan
    if not plans:
        raise ContractError("no V2 Delivery Plans found")
    tasks: Dict[str, Dict[str, Any]] = {}
    for path in sorted((agents / "tasks").glob("XT-*.json")):
        task = load_json(path)
        _validate_task(task, gate_policy["gates"], risk_routing)
        task_id = task["id"]
        if task_id in tasks:
            raise ContractError("duplicate task id {}".format(task_id))
        tasks[task_id] = task
    if not tasks:
        raise ContractError("no V2 TaskSpecs found")
    contracts = ContractSet(
        root=root,
        manifest=manifest,
        plans=plans,
        tasks=tasks,
        gate_policy=gate_policy,
        risk_routing=risk_routing,
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
