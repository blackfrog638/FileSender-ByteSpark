#!/usr/bin/env python3

"""Validate the pinned P1 dependency graph without resolving the network."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path
from typing import Optional


VCPKG_COMMIT = "17f35ad2418007a895ced8a4cece4ab34068a58d"
EXPECTED_VERSIONS = {
    "asio": "1.38.2",
    "openssl": "3.5.7",
    "utf8proc": "2.11.3",
}
OVERLAY_PORTS = {"asio", "openssl"}
TRIPLETS = {
    "xnn-arm64-linux-static.cmake",
    "xnn-arm64-osx-static.cmake",
    "xnn-universal-osx-static.cmake",
    "xnn-x64-linux-static.cmake",
    "xnn-x64-osx-static.cmake",
    "xnn-x64-windows-static.cmake",
}


class DependencyError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise DependencyError(message)


def read_json(path: Path) -> dict[str, object]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise DependencyError(f"cannot parse {path}: {error}") from error
    require(isinstance(value, dict), f"{path} must contain a JSON object")
    return value


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(64 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_manifest(root: Path) -> dict[str, dict[str, str]]:
    versions = read_json(root / "cmake/dependencies/versions.json")
    vcpkg = versions.get("vcpkg")
    dependencies = versions.get("dependencies")
    require(isinstance(vcpkg, dict), "versions.json is missing vcpkg")
    require(
        vcpkg.get("commit") == VCPKG_COMMIT,
        "versions.json vcpkg commit drifted",
    )
    require(
        vcpkg.get("repository") == "https://github.com/microsoft/vcpkg.git",
        "versions.json vcpkg repository drifted",
    )
    require(isinstance(dependencies, dict), "versions.json dependencies missing")
    require(
        set(dependencies) == set(EXPECTED_VERSIONS),
        "versions.json dependency set drifted",
    )

    normalized: dict[str, dict[str, str]] = {}
    for name, expected_version in EXPECTED_VERSIONS.items():
        dependency = dependencies[name]
        require(isinstance(dependency, dict), f"{name} metadata must be an object")
        require(
            dependency.get("version") == expected_version,
            f"{name} version drifted",
        )
        for algorithm, length in (("sha256", 64), ("sha512", 128)):
            digest = dependency.get(algorithm)
            require(
                isinstance(digest, str)
                and len(digest) == length
                and re.fullmatch(r"[0-9a-f]+", digest) is not None,
                f"{name} {algorithm} is not canonical",
            )
        license_value = dependency.get("license")
        license_digest = dependency.get("license_sha256")
        require(isinstance(license_value, str), f"{name} license path missing")
        license_path = root / license_value
        require(license_path.is_file(), f"{name} license file missing")
        require(
            isinstance(license_digest, str)
            and sha256(license_path) == license_digest,
            f"{name} license hash drifted",
        )
        normalized[name] = {
            key: str(dependency[key])
            for key in ("version", "url", "sha256", "sha512")
        }

    manifest = read_json(root / "vcpkg.json")
    require(
        manifest.get("builtin-baseline") == VCPKG_COMMIT,
        "vcpkg.json baseline drifted",
    )
    manifest_dependencies = manifest.get("dependencies")
    require(
        isinstance(manifest_dependencies, list),
        "vcpkg.json dependencies must be a list",
    )
    declared = {
        item.get("name"): item
        for item in manifest_dependencies
        if isinstance(item, dict)
    }
    require(
        set(declared) == set(EXPECTED_VERSIONS),
        "vcpkg.json dependency set drifted",
    )
    for name, item in declared.items():
        require(
            item.get("default-features") is False,
            f"{name} must disable undeclared default features",
        )
    overrides = manifest.get("overrides")
    require(isinstance(overrides, list), "vcpkg.json overrides must be a list")
    override_versions = {
        item.get("name"): item.get("version")
        for item in overrides
        if isinstance(item, dict)
    }
    require(
        override_versions == EXPECTED_VERSIONS,
        "vcpkg.json exact overrides drifted",
    )

    configuration = read_json(root / "vcpkg-configuration.json")
    require(
        "default-registry" not in configuration,
        "vcpkg configuration must not override the manifest baseline",
    )
    require(
        configuration.get("overlay-ports") == ["cmake/vcpkg-overlay"],
        "overlay port root drifted",
    )
    return normalized


def validate_overlays(
    root: Path, dependencies: dict[str, dict[str, str]]
) -> None:
    overlay_root = root / "cmake/vcpkg-overlay"
    actual_ports = {path.name for path in overlay_root.iterdir() if path.is_dir()}
    require(actual_ports == OVERLAY_PORTS, "overlay port set drifted")
    for name in sorted(OVERLAY_PORTS):
        port = overlay_root / name
        metadata = read_json(port / "vcpkg.json")
        require(
            metadata.get("name") == name
            and metadata.get("version") == dependencies[name]["version"],
            f"{name} overlay metadata drifted",
        )
        portfile = (port / "portfile.cmake").read_text(encoding="utf-8")
        require(
            dependencies[name]["url"] in portfile,
            f"{name} overlay source URL drifted",
        )
        require(
            dependencies[name]["sha512"] in portfile,
            f"{name} overlay source SHA-512 drifted",
        )
        require(
            "vcpkg_download_distfile" in portfile
            and "HEAD_REF" not in portfile,
            f"{name} overlay does not use a fixed release archive",
        )


def validate_triplets(root: Path) -> None:
    triplet_root = root / "cmake/triplets"
    actual = {path.name for path in triplet_root.glob("*.cmake")}
    require(actual == TRIPLETS, "project vcpkg triplet set drifted")
    for name in sorted(actual):
        text = (triplet_root / name).read_text(encoding="utf-8")
        require(
            "set(VCPKG_LIBRARY_LINKAGE static)" in text,
            f"{name} permits dynamic dependencies",
        )
        require(
            "set(VCPKG_CRT_LINKAGE dynamic)" in text,
            f"{name} does not preserve the application CRT",
        )


def validate_build_entrypoints(root: Path) -> None:
    root_cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    require(
        "include(cmake/XnnVcpkg.cmake)" in root_cmake,
        "root CMake does not install the pinned toolchain",
    )
    require(
        "include(cmake/XnnDependencies.cmake)" in root_cmake,
        "root CMake does not resolve pinned dependency targets",
    )
    for relative in (
        "apps/desktop/linux/CMakeLists.txt",
        "apps/desktop/windows/CMakeLists.txt",
    ):
        text = (root / relative).read_text(encoding="utf-8")
        require(
            "XnnVcpkg.cmake" in text,
            f"{relative} does not install the pinned toolchain",
        )
        require(
            "XnnDependencies.cmake" in text,
            f"{relative} does not resolve pinned dependency targets",
        )
    macos_script = (
        root / "apps/desktop/macos/build_native.sh"
    ).read_text(encoding="utf-8")
    require(
        "vcpkg_bootstrap.sh" in macos_script,
        "macOS Flutter build does not bootstrap pinned vcpkg",
    )


def validate_vcpkg_checkout(
    root: Path,
    vcpkg_root: Path,
    dependencies: dict[str, dict[str, str]],
) -> None:
    require((vcpkg_root / ".git").is_dir(), "vcpkg checkout is not a Git clone")
    result = subprocess.run(
        ["git", "-C", str(vcpkg_root), "rev-parse", "HEAD"],
        check=True,
        capture_output=True,
        text=True,
    )
    require(result.stdout.strip() == VCPKG_COMMIT, "vcpkg checkout commit drifted")
    utf8proc = read_json(vcpkg_root / "ports/utf8proc/vcpkg.json")
    require(
        utf8proc.get("version") == EXPECTED_VERSIONS["utf8proc"],
        "pinned registry does not provide utf8proc 2.11.3",
    )
    portfile = (
        vcpkg_root / "ports/utf8proc/portfile.cmake"
    ).read_text(encoding="utf-8")
    require(
        dependencies["utf8proc"]["sha512"] in portfile,
        "pinned registry utf8proc SHA-512 drifted",
    )


def validate(root: Path, vcpkg_root: Optional[Path] = None) -> None:
    dependencies = validate_manifest(root)
    validate_overlays(root, dependencies)
    validate_triplets(root)
    validate_build_entrypoints(root)
    if vcpkg_root is not None:
        validate_vcpkg_checkout(root, vcpkg_root, dependencies)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).parents[2])
    parser.add_argument("--vcpkg-root", type=Path)
    args = parser.parse_args()
    try:
        validate(args.root.resolve(), args.vcpkg_root)
    except (DependencyError, OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"Dependency manifest error: {error}") from error
    print("Pinned dependency manifest checks passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
