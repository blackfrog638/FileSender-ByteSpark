#!/usr/bin/env python3
"""Regression-test security word-list line-ending canonicalization."""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


FIXTURE_ROOT = Path(__file__).resolve().parent
VALIDATOR = FIXTURE_ROOT / "validate_vectors.py"
MANIFEST = FIXTURE_ROOT / "vectors.json"
WORDLIST = FIXTURE_ROOT / "wordlist.txt"
SUCCESS_SUMMARY = (
    "Validated 17 positive and 37 negative "
    "XnnTransfer v1 security-profile vectors."
)


def run_fixture(wordlist_bytes: bytes) -> subprocess.CompletedProcess[str]:
    with tempfile.TemporaryDirectory(prefix="xnn-wordlist-") as directory:
        fixture_root = Path(directory)
        manifest = fixture_root / "vectors.json"
        shutil.copyfile(MANIFEST, manifest)
        (fixture_root / "wordlist.txt").write_bytes(wordlist_bytes)
        return subprocess.run(
            [sys.executable, str(VALIDATOR), str(manifest)],
            check=False,
            capture_output=True,
            text=True,
        )


def require_success(name: str, wordlist_bytes: bytes) -> None:
    result = run_fixture(wordlist_bytes)
    if result.returncode != 0 or SUCCESS_SUMMARY not in result.stdout:
        raise RuntimeError(
            f"{name} word list failed with exit {result.returncode}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )


def main() -> int:
    source = WORDLIST.read_bytes()
    canonical = source.replace(b"\r\n", b"\n")
    if b"\r" in canonical:
        raise RuntimeError("source word list contains a bare carriage return")

    require_success("LF", canonical)
    require_success("CRLF", canonical.replace(b"\n", b"\r\n"))

    bare_cr = canonical.replace(b"\n", b"\r", 1)
    result = run_fixture(bare_cr)
    if (
        result.returncode == 0
        or "WORDLIST_MISMATCH" not in result.stderr
        or "bare carriage return" not in result.stderr
    ):
        raise RuntimeError(
            "bare-CR word list did not fail closed\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )

    print("Security word-list LF/CRLF canonicalization regression passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
