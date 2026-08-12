#!/usr/bin/env python3
"""Harness V2 contract validation CLI."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Optional, Sequence

from model import ContractError, approve_plan, load_contracts


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    subparsers = parser.add_subparsers(dest="command", required=True)

    subparsers.add_parser("contracts", help="validate all V2 contracts")

    approval = subparsers.add_parser(
        "approve-plan", help="approve a draft using configured owner identity"
    )
    approval.add_argument("plan", type=Path)
    approval.add_argument("--at", required=True)
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "contracts":
            contracts = load_contracts(args.root)
            print(
                json.dumps(
                    {
                        "harness_version": 2,
                        "plans": len(contracts.plans),
                        "tasks": len(contracts.tasks),
                        "gates": len(contracts.gates),
                        "status": "valid",
                    },
                    sort_keys=True,
                )
            )
            return 0
        if args.command == "approve-plan":
            path = args.plan
            if not path.is_absolute():
                path = args.root / path
            digest = approve_plan(args.root, path, args.at)
            print(digest)
            return 0
    except ContractError as error:
        print("Harness V2 contract error:\n{}".format(error), file=sys.stderr)
        return 1
    raise AssertionError("unreachable command")


if __name__ == "__main__":
    raise SystemExit(main())
