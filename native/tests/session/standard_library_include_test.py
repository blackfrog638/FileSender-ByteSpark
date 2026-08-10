#!/usr/bin/env python3

from pathlib import Path
import sys


FAILURE = (
    "FAILED: session wire must directly include iterator "
    "for std::back_inserter"
)


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: standard_library_include_test.py <wire.cpp>")

    source = Path(sys.argv[1]).read_text(encoding="utf-8")
    direct_includes = {
        line.strip()
        for line in source.splitlines()
        if line.lstrip().startswith("#include")
    }
    if "#include <iterator>" not in direct_includes:
        print(FAILURE)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
