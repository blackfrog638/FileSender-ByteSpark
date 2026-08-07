#!/usr/bin/env python3

"""Prove breaking mutations fail and additive ABI v1 evolution passes."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import tempfile
from pathlib import Path


def replace_once(source: str, old: str, new: str) -> str:
    if source.count(old) != 1:
        raise SystemExit(f"mutation anchor must occur exactly once: {old!r}")
    return source.replace(old, new, 1)


def mutation_cases(header: str) -> dict[str, str]:
    return {
        "macro_value": replace_once(
            header,
            "#define XNN_TRANSFER_EVENT_QUEUE_CAPACITY 64u",
            "#define XNN_TRANSFER_EVENT_QUEUE_CAPACITY 65u",
        ),
        "struct_offset": replace_once(
            header,
            """typedef struct xnn_transfer_engine_config {
  size_t struct_size;
  uint32_t abi_version;
  uint32_t reserved;
} xnn_transfer_engine_config;""",
            """typedef struct xnn_transfer_engine_config {
  uint32_t abi_version;
  size_t struct_size;
  uint32_t reserved;
} xnn_transfer_engine_config;""",
        ),
        "field_type": replace_once(
            header,
            """typedef struct xnn_transfer_engine_config {
  size_t struct_size;
  uint32_t abi_version;
  uint32_t reserved;
} xnn_transfer_engine_config;""",
            """typedef struct xnn_transfer_engine_config {
  size_t struct_size;
  uint32_t abi_version;
  int32_t reserved;
} xnn_transfer_engine_config;""",
        ),
        "function_signature": replace_once(
            header,
            """XNN_TRANSFER_API xnn_transfer_status
xnn_transfer_engine_start(xnn_transfer_engine* engine);""",
            """XNN_TRANSFER_API void
xnn_transfer_engine_start(xnn_transfer_engine* engine);""",
        ),
    }


def additive_case(header: str) -> str:
    extended = replace_once(
        header,
        """typedef struct xnn_transfer_engine_config {
  size_t struct_size;
  uint32_t abi_version;
  uint32_t reserved;
} xnn_transfer_engine_config;""",
        """typedef struct xnn_transfer_engine_config {
  size_t struct_size;
  uint32_t abi_version;
  uint32_t reserved;
  uint64_t additive_tail;
} xnn_transfer_engine_config;""",
    )
    return replace_once(
        extended,
        """XNN_TRANSFER_API xnn_transfer_status xnn_transfer_engine_poll_event(
    xnn_transfer_engine* engine, xnn_transfer_event* out_event);""",
        """XNN_TRANSFER_API xnn_transfer_status xnn_transfer_engine_poll_event(
    xnn_transfer_engine* engine, xnn_transfer_event* out_event);

XNN_TRANSFER_API uint32_t xnn_transfer_additive_symbol(void);""",
    )


def configure_and_build(
    root: Path,
    cmake: str,
    generator: str,
) -> subprocess.CompletedProcess[str]:
    build = root / "build"
    configure = subprocess.run(
        [cmake, "-S", str(root), "-B", str(build), "-G", generator],
        check=False,
        capture_output=True,
        text=True,
    )
    if configure.returncode != 0:
        raise SystemExit(
            "negative ABI fixture failed to configure:\n"
            + configure.stdout
            + configure.stderr
        )
    return subprocess.run(
        [cmake, "--build", str(build)],
        check=False,
        capture_output=True,
        text=True,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--header", type=Path, required=True)
    parser.add_argument("--assertions", type=Path, required=True)
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--generator", required=True)
    args = parser.parse_args()

    frozen_header = args.header.read_text(encoding="utf-8")
    assertions = args.assertions.read_text(encoding="utf-8")
    cases = mutation_cases(frozen_header)

    with tempfile.TemporaryDirectory(prefix="xnn-transfer-abi-negative-") as value:
        temporary = Path(value)
        fixtures = {"additive_extension": (additive_case(frozen_header), True)}
        fixtures.update(
            {name: (mutated_header, False) for name, mutated_header in cases.items()}
        )
        for name, (mutated_header, should_compile) in fixtures.items():
            root = temporary / name
            include = root / "include" / "xnn_transfer"
            include.mkdir(parents=True)
            (include / "c_api.h").write_text(mutated_header, encoding="utf-8")
            (root / "v1_compat_assertions.hpp").write_text(
                assertions,
                encoding="utf-8",
            )
            (root / "main.cpp").write_text(
                '#include "xnn_transfer/c_api.h"\n'
                '#include "v1_compat_assertions.hpp"\n'
                "int main() { return 0; }\n",
                encoding="utf-8",
            )
            (root / "CMakeLists.txt").write_text(
                """cmake_minimum_required(VERSION 3.24)
project(XnnTransferAbiNegative LANGUAGES CXX)
add_executable(abi_negative main.cpp)
target_compile_features(abi_negative PRIVATE cxx_std_20)
target_include_directories(abi_negative PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}"
)
""",
                encoding="utf-8",
            )
            result = configure_and_build(root, args.cmake, args.generator)
            if should_compile and result.returncode != 0:
                raise SystemExit(
                    "additive ABI fixture failed to compile:\n"
                    + result.stdout
                    + result.stderr
                )
            if not should_compile and result.returncode == 0:
                raise SystemExit(f"incompatible ABI fixture compiled: {name}")
            shutil.rmtree(root / "build", ignore_errors=True)

    print(
        "Accepted additive ABI evolution and rejected "
        f"{len(cases)} incompatible ABI v1 compile fixtures."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
