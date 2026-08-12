#!/usr/bin/env python3
"""Harness V2 Gate DAG planning and risk selection."""

from __future__ import annotations

import fnmatch
from dataclasses import dataclass
from typing import Any, Dict, List, Mapping, Optional, Sequence, Set, Tuple

from model import ContractSet, canonical_sha256, patterns_overlap


class GatePlanError(RuntimeError):
    """Raised when required Gate coverage cannot be planned."""


@dataclass(frozen=True)
class GatePlan:
    task_id: str
    phase: str
    requested: Tuple[str, ...]
    leaves: Tuple[str, ...]
    reasons: Mapping[str, Tuple[str, ...]]
    digest: str


def _criterion_gates(contracts: ContractSet, task_id: str) -> Dict[str, Set[str]]:
    task = contracts.tasks[task_id]
    wanted = set(task["criteria"])
    result: Dict[str, Set[str]] = {}
    plan = contracts.plans[task["plan"]]
    for requirement in plan["requirements"]:
        for criterion in requirement["criteria"]:
            if criterion["id"] not in wanted:
                continue
            for gate_id in criterion["evidence"]["gates"]:
                result.setdefault(gate_id, set()).add(
                    "criterion:{}".format(criterion["id"])
                )
    return result


def _path_matches(path: str, pattern: str) -> bool:
    return fnmatch.fnmatchcase(path, pattern) or patterns_overlap(path, pattern)


def required_gates(
    contracts: ContractSet,
    task_id: str,
    phase: str,
    changed_paths: Optional[Sequence[str]] = None,
) -> Dict[str, Set[str]]:
    if task_id not in contracts.tasks:
        raise GatePlanError("unknown task {}".format(task_id))
    task = contracts.tasks[task_id]
    gates: Dict[str, Set[str]] = _criterion_gates(contracts, task_id)
    tdd_gate = task["tdd"]["gate"]
    if tdd_gate is not None:
        gates.setdefault(tdd_gate, set()).add("tdd:{}".format(task["tdd"]["mode"]))
    minimums = contracts.risk_routing["phase_minimums"]
    if phase not in minimums:
        raise GatePlanError("unknown verification phase {}".format(phase))
    for gate_id in minimums[phase]:
        gates.setdefault(gate_id, set()).add("phase:{}".format(phase))
    impacted = list(changed_paths or task["owned_paths"])
    for index, rule in enumerate(contracts.risk_routing["path_rules"]):
        if any(
            _path_matches(path, pattern)
            for path in impacted
            for pattern in rule["paths"]
        ):
            for gate_id in rule["required_gates"]:
                gates.setdefault(gate_id, set()).add("path-rule:{}".format(index))
    return gates


def _expand_gate(
    gate_id: str,
    policy: Mapping[str, Any],
    visiting: Set[str],
    leaves: List[str],
    seen: Set[str],
) -> None:
    if gate_id in visiting:
        raise GatePlanError("Gate DAG cycle at {}".format(gate_id))
    if gate_id not in policy:
        raise GatePlanError("unknown Gate {}".format(gate_id))
    gate = policy[gate_id]
    if gate["command"] is not None:
        if gate_id not in seen:
            seen.add(gate_id)
            leaves.append(gate_id)
        return
    visiting.add(gate_id)
    for dependency in gate["aggregate"]:
        _expand_gate(dependency, policy, visiting, leaves, seen)
    visiting.remove(gate_id)


def plan_gates(
    contracts: ContractSet,
    task_id: str,
    phase: str,
    changed_paths: Optional[Sequence[str]] = None,
) -> GatePlan:
    reasons = required_gates(contracts, task_id, phase, changed_paths)
    requested = tuple(sorted(reasons))
    leaves: List[str] = []
    seen: Set[str] = set()
    for gate_id in requested:
        _expand_gate(gate_id, contracts.gates, set(), leaves, seen)
    leaf_reasons: Dict[str, Set[str]] = {gate_id: set() for gate_id in leaves}
    for requested_gate, requested_reasons in reasons.items():
        requested_leaves: List[str] = []
        _expand_gate(
            requested_gate,
            contracts.gates,
            set(),
            requested_leaves,
            set(),
        )
        for leaf in requested_leaves:
            leaf_reasons[leaf].update(requested_reasons)
            if leaf != requested_gate:
                leaf_reasons[leaf].add("aggregate:{}".format(requested_gate))
    canonical_reasons = {
        gate_id: tuple(sorted(values))
        for gate_id, values in sorted(leaf_reasons.items())
    }
    payload = {
        "task_id": task_id,
        "phase": phase,
        "requested": list(requested),
        "leaves": leaves,
        "reasons": {
            gate_id: list(values)
            for gate_id, values in canonical_reasons.items()
        },
    }
    return GatePlan(
        task_id=task_id,
        phase=phase,
        requested=requested,
        leaves=tuple(leaves),
        reasons=canonical_reasons,
        digest=canonical_sha256(payload),
    )


def single_gate_plan(
    contracts: ContractSet, task_id: str, phase: str, gate_id: str
) -> GatePlan:
    leaves: List[str] = []
    _expand_gate(gate_id, contracts.gates, set(), leaves, set())
    reasons = {leaf: ("focused:{}".format(gate_id),) for leaf in leaves}
    payload = {
        "task_id": task_id,
        "phase": phase,
        "requested": [gate_id],
        "leaves": leaves,
        "reasons": {key: list(value) for key, value in reasons.items()},
    }
    return GatePlan(
        task_id,
        phase,
        (gate_id,),
        tuple(leaves),
        reasons,
        canonical_sha256(payload),
    )
