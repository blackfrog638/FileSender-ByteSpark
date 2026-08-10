#!/usr/bin/env python3

from pathlib import Path
import sys


FAILURE = (
    "FAILED: Windows native test support must isolate min and max macros"
)


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit(
            "usage: windows_macro_policy_test.py <session-test-support.hpp>"
        )

    lines = Path(sys.argv[1]).read_text(encoding="utf-8").splitlines()
    directives = [line.strip() for line in lines]
    try:
        first_include = next(
            index
            for index, directive in enumerate(directives)
            if directive.startswith("#include")
        )
    except StopIteration:
        print(FAILURE)
        return 1

    required_pairs = (
        ("#ifndef NOMINMAX", "#define NOMINMAX"),
        ("#ifdef min", "#undef min"),
        ("#ifdef max", "#undef max"),
    )
    for guard, action in required_pairs:
        try:
            guard_index = directives.index(guard)
            action_index = directives.index(action)
        except ValueError:
            print(FAILURE)
            return 1
        if not guard_index < action_index < first_include:
            print(FAILURE)
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
