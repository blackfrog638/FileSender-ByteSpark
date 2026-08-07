#!/usr/bin/env python3

"""Load the integration-owned verification gate registry."""

from __future__ import annotations

import re
from pathlib import Path

GATE_NAME = re.compile(r"^[a-z][a-z0-9_]*$")
COMMAND_ENTRY = re.compile(r"^  ([a-z][a-z0-9_]*):\s+(.+?)\s*$")


class GateRegistryError(RuntimeError):
    pass


def load_gate_registry(path: Path) -> dict[str, str]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise GateRegistryError(f"cannot read gate registry: {error}") from error

    commands: dict[str, str] = {}
    in_commands = False
    for line_number, line in enumerate(lines, start=1):
        if line == "commands:":
            if in_commands or commands:
                raise GateRegistryError("manifest contains duplicate commands sections")
            in_commands = True
            continue
        if not in_commands:
            continue
        if line and not line.startswith((" ", "\t")):
            break
        if not line.strip():
            continue
        match = COMMAND_ENTRY.fullmatch(line)
        if match is None:
            raise GateRegistryError(
                f"invalid command entry at manifest line {line_number}"
            )
        gate, command = match.groups()
        if not GATE_NAME.fullmatch(gate):
            raise GateRegistryError(f"invalid gate name: {gate}")
        if gate in commands:
            raise GateRegistryError(f"duplicate gate name: {gate}")
        commands[gate] = command

    if "verify" not in commands:
        raise GateRegistryError("gate registry must define verify")
    if len(commands.values()) != len(set(commands.values())):
        raise GateRegistryError("gate registry commands must be unique")
    return commands
