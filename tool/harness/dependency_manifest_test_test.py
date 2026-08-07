#!/usr/bin/env python3

"""Negative fixtures for pinned dependency manifest validation."""

from __future__ import annotations

import json
import shutil
import tempfile
import unittest
from pathlib import Path

import dependency_manifest_test as dependency


REPOSITORY = Path(__file__).parents[2]


class DependencyManifestTest(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(
            prefix="xnn-transfer-dependency-manifest-"
        )
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        for directory in ("cmake", "third_party"):
            shutil.copytree(REPOSITORY / directory, self.root / directory)
        for relative in (
            "CMakeLists.txt",
            "vcpkg.json",
            "vcpkg-configuration.json",
            "apps/desktop/linux/CMakeLists.txt",
            "apps/desktop/windows/CMakeLists.txt",
            "apps/desktop/macos/build_native.sh",
        ):
            destination = self.root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(REPOSITORY / relative, destination)

    def mutate_json(self, relative: str, update) -> None:
        path = self.root / relative
        value = json.loads(path.read_text(encoding="utf-8"))
        update(value)
        path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")

    def test_accepted_fixture(self) -> None:
        dependency.validate(self.root)

    def test_rejects_floating_baseline(self) -> None:
        self.mutate_json(
            "vcpkg.json",
            lambda value: value.update({"builtin-baseline": "0" * 40}),
        )
        with self.assertRaisesRegex(dependency.DependencyError, "baseline"):
            dependency.validate(self.root)

    def test_rejects_source_hash_drift(self) -> None:
        self.mutate_json(
            "cmake/dependencies/versions.json",
            lambda value: value["dependencies"]["openssl"].update(
                {"sha512": "0" * 128}
            ),
        )
        with self.assertRaisesRegex(dependency.DependencyError, "OpenSSL|openssl"):
            dependency.validate(self.root)

    def test_rejects_license_drift(self) -> None:
        path = self.root / "third_party/licenses/asio-1.38.2.txt"
        path.write_text("changed\n", encoding="utf-8")
        with self.assertRaisesRegex(dependency.DependencyError, "license hash"):
            dependency.validate(self.root)

    def test_rejects_dynamic_triplet(self) -> None:
        path = self.root / "cmake/triplets/xnn-x64-linux-static.cmake"
        text = path.read_text(encoding="utf-8")
        path.write_text(
            text.replace(
                "set(VCPKG_LIBRARY_LINKAGE static)",
                "set(VCPKG_LIBRARY_LINKAGE dynamic)",
            ),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(dependency.DependencyError, "dynamic"):
            dependency.validate(self.root)

    def test_rejects_system_fallback_entrypoint(self) -> None:
        path = self.root / "CMakeLists.txt"
        text = path.read_text(encoding="utf-8")
        path.write_text(
            text.replace("include(cmake/XnnVcpkg.cmake)\n", ""),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(dependency.DependencyError, "toolchain"):
            dependency.validate(self.root)

    def test_rejects_flutter_dependency_resolution_bypass(self) -> None:
        path = self.root / "apps/desktop/windows/CMakeLists.txt"
        text = path.read_text(encoding="utf-8")
        path.write_text(
            text.replace(
                'include("${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/'
                'XnnDependencies.cmake")\n',
                "",
            ),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(dependency.DependencyError, "dependency targets"):
            dependency.validate(self.root)


if __name__ == "__main__":
    unittest.main()
