#!/usr/bin/env python3

"""Resolve the frozen ABI v1 exports from the actual shared library."""

from __future__ import annotations

import argparse
import ctypes
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("library", type=Path)
    parser.add_argument("required_exports", type=Path)
    args = parser.parse_args()

    exports = [
        line.strip()
        for line in args.required_exports.read_text(encoding="ascii").splitlines()
        if line.strip()
    ]
    if exports != sorted(set(exports)):
        raise SystemExit("required ABI exports must be sorted and unique")

    library = ctypes.CDLL(str(args.library.resolve()))
    missing = [name for name in exports if not hasattr(library, name)]
    if missing:
        raise SystemExit("missing ABI v1 exports: " + ", ".join(missing))

    abi_version = library.xnn_transfer_abi_version
    abi_version.argtypes = []
    abi_version.restype = ctypes.c_uint32
    if abi_version() != 1:
        raise SystemExit("loaded library does not report ABI v1")

    print(f"Resolved {len(exports)} required ABI v1 exports.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
