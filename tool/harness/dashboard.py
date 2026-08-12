#!/usr/bin/env python3
"""Render a read-only Harness V2 status dashboard."""

from __future__ import annotations

import argparse
import html
from pathlib import Path
from typing import Optional, Sequence

import model
import state


def render(root: Path, output: Path, remote: Optional[str] = None) -> None:
    contracts = model.load_contracts(root)
    store = state.StateStore(contracts, remote=remote)
    snapshots = store.list()
    rows = []
    for task_id in sorted(contracts.tasks):
        task = contracts.tasks[task_id]
        snapshot = snapshots[task_id]
        rows.append(
            "<tr><td>{}</td><td>{}</td><td>{}</td><td>{}</td></tr>".format(
                html.escape(task_id),
                html.escape(task["title"]),
                html.escape(snapshot.state),
                html.escape(task["workstream"]),
            )
        )
    document = """<!doctype html>
<html lang="en">
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>XnnTransfer Harness V2</title>
<style>
body {{ font: 15px system-ui; margin: 2rem; color: #172033; }}
.summary {{ display: flex; gap: 1rem; margin: 1rem 0; }}
.card {{ border: 1px solid #d6dbe6; border-radius: 8px; padding: .8rem 1rem; }}
table {{ border-collapse: collapse; width: 100%; }}
th, td {{ border-bottom: 1px solid #d6dbe6; padding: .6rem; text-align: left; }}
</style>
<h1>Harness V2</h1>
<p>Derived read-only view. Git contracts and remote state refs remain authoritative.</p>
<div class="summary">
  <div class="card">Plans: {plans}</div>
  <div class="card">Active TaskSpecs: {tasks}</div>
  <div class="card">Legacy accepted: {legacy}</div>
</div>
<table>
<thead><tr><th>Task</th><th>Title</th><th>State</th><th>Workstream</th></tr></thead>
<tbody>{rows}</tbody>
</table>
</html>
""".format(
        plans=len(contracts.plans),
        tasks=len(contracts.tasks),
        legacy=len(contracts.legacy_accepted),
        rows="".join(rows),
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(document, encoding="utf-8")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("build/dashboard/index.html"),
    )
    parser.add_argument("--remote")
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = _parser().parse_args(argv)
    output = args.output
    if not output.is_absolute():
        output = args.root / output
    render(args.root.resolve(), output, args.remote)
    print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
