#!/usr/bin/env python3

"""Enforce the repository's Flutter and native dependency direction."""

from __future__ import annotations

import argparse
import json
import re
import shlex
import sys
from pathlib import Path, PurePosixPath
from typing import Mapping

sys.dont_write_bytecode = True
HARNESS_DIR = Path(__file__).resolve().parent
if str(HARNESS_DIR) not in sys.path:
    sys.path.insert(0, str(HARNESS_DIR))
from module_inventory import (
    Module,
    Violation,
    load_records,
    parse_modules,
    schema_violation,
    target_links,
    validate_module_inventory,
    validate_temporary_leases,
)

DART_DIRECTIVE = re.compile(
    r"""(?m)^\s*(?:import|export|part)\s+(['"])([^'"]+)\1"""
)
CPP_INCLUDE = re.compile(r'(?m)^\s*#\s*include\s*[<"]([^>"]+)[>"]')
CMAKE_LINK = re.compile(
    r"target_link_libraries\s*\(\s*([A-Za-z0-9_]+)(.*?)\)",
    flags=re.IGNORECASE | re.DOTALL,
)
CMAKE_INCLUDE_DIRECTORIES = re.compile(
    r"target_include_directories\s*\(\s*([A-Za-z0-9_]+)(.*?)\)",
    flags=re.IGNORECASE | re.DOTALL,
)
PROJECT_TARGET = re.compile(r"\bxnn_transfer_[a-z0-9_]+\b")

FLUTTER_LAYER_LINKS = {
    "main": {"app"},
    "app": {
        "app",
        "core_native",
        "domain",
        "application",
        "presentation",
    },
    "core_native": {"core_native", "domain"},
    "domain": {"domain"},
    "application": {"application", "domain"},
    "presentation": {"presentation", "application", "domain"},
}

PUBLIC_CORE_FORBIDDEN_INCLUDES = (
    "asio/",
    "boost/asio",
    "openssl/",
    "utf8proc",
    "windows.h",
    "security/security.h",
    "libsecret",
    "filesystem",
    "dart_api",
    "flutter",
)

def line_number(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def normalize_relative(path: PurePosixPath) -> PurePosixPath:
    parts: list[str] = []
    for part in path.parts:
        if part in {"", "."}:
            continue
        if part == "..":
            if parts:
                parts.pop()
            else:
                parts.append(part)
            continue
        parts.append(part)
    return PurePosixPath(*parts)


def flutter_layer(path: PurePosixPath) -> str | None:
    parts = path.parts
    if parts == ("main.dart",):
        return "main"
    if parts and parts[0] == "app":
        return "app"
    if len(parts) >= 2 and parts[:2] == ("core", "native"):
        return "core_native"
    if len(parts) >= 3 and parts[0] == "features":
        if parts[2] in {"domain", "application", "presentation"}:
            return parts[2]
    return None


def resolve_flutter_uri(
    source: PurePosixPath, uri: str
) -> PurePosixPath | None:
    prefix = "package:xnn_transfer/"
    if uri.startswith(prefix):
        return normalize_relative(PurePosixPath(uri.removeprefix(prefix)))
    if ":" not in uri and not uri.startswith("/"):
        return normalize_relative(source.parent / uri)
    return None


def scan_flutter_text(path: PurePosixPath, text: str) -> list[Violation]:
    violations: list[Violation] = []
    source_layer = flutter_layer(path)
    display_path = f"apps/desktop/lib/{path.as_posix()}"
    if source_layer is None:
        violations.append(
            Violation(
                display_path,
                1,
                "production Dart file is outside a declared architecture layer",
            )
        )
        return violations

    for match in DART_DIRECTIVE.finditer(text):
        uri = match.group(2)
        line = line_number(text, match.start())
        if uri == "dart:ffi" and source_layer != "core_native":
            violations.append(
                Violation(
                    display_path,
                    line,
                    "dart:ffi is only allowed in lib/core/native",
                )
            )
        if source_layer == "domain" and uri.startswith("package:flutter/"):
            violations.append(
                Violation(
                    display_path,
                    line,
                    "domain code must not depend on Flutter",
                )
            )

        target = resolve_flutter_uri(path, uri)
        if target is None:
            continue
        target_layer = flutter_layer(target)
        if target_layer is None:
            violations.append(
                Violation(
                    display_path,
                    line,
                    f"import target is outside a declared layer: {uri}",
                )
            )
            continue
        if target_layer not in FLUTTER_LAYER_LINKS[source_layer]:
            violations.append(
                Violation(
                    display_path,
                    line,
                    f"{source_layer} must not depend on {target_layer}: {uri}",
                )
            )
    return violations


def scan_native_text(path: PurePosixPath, text: str) -> list[Violation]:
    violations: list[Violation] = []
    display_path = path.as_posix()
    is_bridge = path.parts[:3] == ("native", "src", "bridge")
    is_public_core = path.parts[:4] == (
        "native",
        "include",
        "xnn_transfer",
        "core",
    )

    for match in CPP_INCLUDE.finditer(text):
        include = match.group(1).replace("\\", "/")
        lowered = include.lower()
        line = line_number(text, match.start())
        parts = PurePosixPath(include).parts

        if include == "xnn_transfer/c_api.h" and not is_bridge:
            violations.append(
                Violation(
                    display_path,
                    line,
                    "the public C ABI may only be included by native/src/bridge",
                )
            )
        if (
            ".." in parts
            or lowered.startswith("/")
            or re.match(r"^[a-z]:/", lowered)
            or lowered.startswith("native/src/")
            or "/src/" in lowered
        ):
            violations.append(
                Violation(
                    display_path,
                    line,
                    f"production code must not include a private source path: {include}",
                )
            )
        if (
            lowered.startswith("apps/desktop/")
            or "dart_api" in lowered
            or "flutter_embedder" in lowered
            or lowered.startswith("flutter/")
        ):
            violations.append(
                Violation(
                    display_path,
                    line,
                    f"native production code must not include Flutter or Dart: {include}",
                )
            )
        if is_public_core and any(
            token in lowered for token in PUBLIC_CORE_FORBIDDEN_INCLUDES
        ):
            violations.append(
                Violation(
                    display_path,
                    line,
                    f"public core headers must not expose infrastructure: {include}",
                )
            )
    return violations


def strip_cmake_comments(text: str) -> str:
    return "\n".join(line.split("#", 1)[0] for line in text.splitlines())


def resolve_cmake_include(
    cmake_path: PurePosixPath, value: str
) -> PurePosixPath | None:
    if value.startswith("$<INSTALL_INTERFACE:") and value.endswith(">"):
        return None
    if value.startswith("$<BUILD_INTERFACE:") and value.endswith(">"):
        value = value.removeprefix("$<BUILD_INTERFACE:")[:-1]

    current_directory = cmake_path.parent.as_posix()
    replacements = {
        "${CMAKE_CURRENT_LIST_DIR}": current_directory,
        "${CMAKE_CURRENT_SOURCE_DIR}": current_directory,
        "${CMAKE_SOURCE_DIR}": ".",
        "${PROJECT_SOURCE_DIR}": ".",
    }
    for variable, replacement in replacements.items():
        value = value.replace(variable, replacement)
    if "$<" in value or "${" in value:
        return None
    if value.startswith("/") or re.match(r"^[a-zA-Z]:[/\\]", value):
        return None
    return normalize_relative(PurePosixPath(value.replace("\\", "/")))


def scan_cmake_include_directories(
    path: PurePosixPath,
    text: str,
    runtime_links: Mapping[str, set[str]],
) -> list[Violation]:
    violations: list[Violation] = []
    for match in CMAKE_INCLUDE_DIRECTORIES.finditer(text):
        target = match.group(1).lower()
        if target not in runtime_links:
            continue
        try:
            tokens = shlex.split(match.group(2), comments=False, posix=True)
        except ValueError:
            continue
        visibility = ""
        for token in tokens:
            upper = token.upper()
            if upper in {"PRIVATE", "PUBLIC", "INTERFACE"}:
                visibility = upper
                continue
            if upper in {"AFTER", "BEFORE", "SYSTEM"}:
                continue
            if visibility not in {"PUBLIC", "INTERFACE"}:
                continue
            for value in token.split(";"):
                resolved = resolve_cmake_include(path, value)
                if resolved is None or resolved.parts[:2] != ("native", "src"):
                    continue
                violations.append(
                    Violation(
                        path.as_posix(),
                        line_number(text, match.start()),
                        f"{target} must not expose {resolved} through "
                        f"{visibility}",
                    )
                )
    return violations


def scan_cmake_text(
    path: PurePosixPath,
    text: str,
    runtime_links: Mapping[str, set[str]] | None = None,
) -> list[Violation]:
    violations: list[Violation] = []
    if runtime_links is None:
        modules, module_violations = parse_modules(
            Path(__file__).resolve().parents[2]
        )
        if module_violations:
            return module_violations
        runtime_links = target_links(modules)
    uncommented = strip_cmake_comments(text)
    violations.extend(
        scan_cmake_include_directories(path, uncommented, runtime_links)
    )
    for match in CMAKE_LINK.finditer(uncommented):
        source = match.group(1).lower()
        allowed = runtime_links.get(source)
        if allowed is None:
            continue
        dependencies = {
            dependency.lower()
            for dependency in PROJECT_TARGET.findall(match.group(2).lower())
        }
        for dependency in sorted(dependencies):
            if dependency == source:
                continue
            if dependency not in allowed:
                violations.append(
                    Violation(
                        path.as_posix(),
                        line_number(uncommented, match.start()),
                        f"{source} must not link {dependency}",
                    )
                )
    return violations


def scan_repository(root: Path) -> list[Violation]:
    modules, violations = parse_modules(root)
    try:
        records = load_records(root)
    except (OSError, json.JSONDecodeError) as error:
        violations.append(
            schema_violation(f"cannot read task records: {error}")
        )
        records = {}
    runtime_links = target_links(modules)
    flutter_root = root / "apps" / "desktop" / "lib"
    for path in sorted(flutter_root.rglob("*.dart")):
        relative = PurePosixPath(path.relative_to(flutter_root).as_posix())
        violations.extend(
            scan_flutter_text(relative, path.read_text(encoding="utf-8"))
        )

    for native_root in (root / "native" / "include", root / "native" / "src"):
        for path in sorted(native_root.rglob("*")):
            if path.suffix not in {".cpp", ".h", ".hpp", ".cc", ".cxx"}:
                continue
            relative = PurePosixPath(path.relative_to(root).as_posix())
            violations.extend(
                scan_native_text(relative, path.read_text(encoding="utf-8"))
            )

    cmake_paths = [root / "native" / "CMakeLists.txt"]
    cmake_paths.extend(sorted((root / "native" / "src").rglob("CMakeLists.txt")))
    for path in cmake_paths:
        relative = PurePosixPath(path.relative_to(root).as_posix())
        violations.extend(
            scan_cmake_text(
                relative,
                path.read_text(encoding="utf-8"),
                runtime_links,
            )
        )
    violations.extend(
        validate_module_inventory(root, modules, cmake_paths, records)
    )
    violations.extend(validate_temporary_leases(root, records))
    return sorted(set(violations))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[2],
    )
    args = parser.parse_args()
    violations = scan_repository(args.root.resolve())
    if violations:
        print("Architecture dependency violations:", file=sys.stderr)
        for violation in violations:
            print(f"- {violation.render()}", file=sys.stderr)
        return 1
    print("Architecture dependency checks passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
