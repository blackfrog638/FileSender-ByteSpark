#!/usr/bin/env python3

"""Enforce the repository's Flutter and native dependency direction."""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path, PurePosixPath


DART_DIRECTIVE = re.compile(
    r"""(?m)^\s*(?:import|export|part)\s+(['"])([^'"]+)\1"""
)
CPP_INCLUDE = re.compile(r'(?m)^\s*#\s*include\s*[<"]([^>"]+)[>"]')
CMAKE_LINK = re.compile(
    r"target_link_libraries\s*\(\s*([A-Za-z0-9_]+)(.*?)\)",
    flags=re.IGNORECASE | re.DOTALL,
)
PROJECT_TARGET = re.compile(r"\bxnn_transfer_[a-z0-9_]+\b")

RUNTIME_TARGET_LINKS = {
    "xnn_transfer_core": {
        "xnn_transfer_discovery",
        "xnn_transfer_identity",
        "xnn_transfer_protocol",
        "xnn_transfer_session",
        "xnn_transfer_storage",
        "xnn_transfer_tls",
        "xnn_transfer_transfer",
    },
    "xnn_transfer_protocol": set(),
    "xnn_transfer_discovery": {"xnn_transfer_protocol"},
    "xnn_transfer_identity": set(),
    "xnn_transfer_tls": {"xnn_transfer_identity"},
    "xnn_transfer_session": {
        "xnn_transfer_identity",
        "xnn_transfer_protocol",
        "xnn_transfer_tls",
    },
    "xnn_transfer_storage": set(),
    "xnn_transfer_transfer": {
        "xnn_transfer_protocol",
        "xnn_transfer_session",
        "xnn_transfer_storage",
    },
}

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


@dataclass(frozen=True, order=True)
class Violation:
    path: str
    line: int
    message: str

    def render(self) -> str:
        return f"{self.path}:{self.line}: {self.message}"


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


def scan_cmake_text(path: PurePosixPath, text: str) -> list[Violation]:
    violations: list[Violation] = []
    uncommented = strip_cmake_comments(text)
    for match in CMAKE_LINK.finditer(uncommented):
        source = match.group(1).lower()
        allowed = RUNTIME_TARGET_LINKS.get(source)
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
    violations: list[Violation] = []
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
            scan_cmake_text(relative, path.read_text(encoding="utf-8"))
        )
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
