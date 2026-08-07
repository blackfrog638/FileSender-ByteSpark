#!/usr/bin/env python3

"""Focused tests for the mechanical architecture dependency gate."""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path, PurePosixPath

import architecture_test as architecture


def messages(violations: list[architecture.Violation]) -> list[str]:
    return [violation.message for violation in violations]


class FlutterDependencyTests(unittest.TestCase):
    def assert_allowed(self, path: str, source: str) -> None:
        self.assertEqual(
            architecture.scan_flutter_text(PurePosixPath(path), source),
            [],
        )

    def assert_rejected(self, path: str, source: str, expected: str) -> None:
        self.assertTrue(
            any(
                expected in message
                for message in messages(
                    architecture.scan_flutter_text(
                        PurePosixPath(path),
                        source,
                    )
                )
            )
        )

    def test_allows_declared_downward_and_composition_dependencies(self) -> None:
        cases = (
            (
                "features/send/domain/model.dart",
                "import 'package:xnn_transfer/features/shared/domain/id.dart';",
            ),
            (
                "features/send/application/controller.dart",
                "import '../domain/model.dart';",
            ),
            (
                "features/send/presentation/page.dart",
                "import '../application/controller.dart';",
            ),
            (
                "core/native/adapter.dart",
                "import 'dart:ffi';\n"
                "import 'package:xnn_transfer/features/send/domain/gateway.dart';",
            ),
            (
                "app/root.dart",
                "import 'package:xnn_transfer/core/native/adapter.dart';\n"
                "import 'package:xnn_transfer/features/send/presentation/page.dart';",
            ),
            ("main.dart", "import 'package:xnn_transfer/app/root.dart';"),
        )
        for path, source in cases:
            with self.subTest(path=path):
                self.assert_allowed(path, source)

    def test_rejects_forbidden_flutter_edges(self) -> None:
        cases = (
            (
                "features/send/domain/model.dart",
                "import '../application/controller.dart';",
                "domain must not depend on application",
            ),
            (
                "features/send/domain/model.dart",
                "import 'package:xnn_transfer/core/native/adapter.dart';",
                "domain must not depend on core_native",
            ),
            (
                "features/send/application/controller.dart",
                "import '../presentation/page.dart';",
                "application must not depend on presentation",
            ),
            (
                "features/send/application/controller.dart",
                "import 'package:xnn_transfer/core/native/adapter.dart';",
                "application must not depend on core_native",
            ),
            (
                "features/send/presentation/page.dart",
                "import 'package:xnn_transfer/core/native/adapter.dart';",
                "presentation must not depend on core_native",
            ),
            (
                "core/native/adapter.dart",
                "import 'package:xnn_transfer/features/send/application/controller.dart';",
                "core_native must not depend on application",
            ),
            (
                "features/send/presentation/page.dart",
                "import 'dart:ffi';",
                "dart:ffi is only allowed",
            ),
            (
                "features/send/domain/model.dart",
                "import 'package:flutter/widgets.dart';",
                "domain code must not depend on Flutter",
            ),
            (
                "misc/helper.dart",
                "import 'dart:async';",
                "outside a declared architecture layer",
            ),
        )
        for path, source, expected in cases:
            with self.subTest(path=path):
                self.assert_rejected(path, source, expected)


class NativeIncludeTests(unittest.TestCase):
    def assert_allowed(self, path: str, source: str) -> None:
        self.assertEqual(
            architecture.scan_native_text(PurePosixPath(path), source),
            [],
        )

    def assert_rejected(self, path: str, source: str, expected: str) -> None:
        self.assertTrue(
            any(
                expected in message
                for message in messages(
                    architecture.scan_native_text(
                        PurePosixPath(path),
                        source,
                    )
                )
            )
        )

    def test_allows_bridge_c_abi_and_domain_standard_library(self) -> None:
        self.assert_allowed(
            "native/src/bridge/c_api.cpp",
            '#include "xnn_transfer/c_api.h"\n#include <mutex>',
        )
        self.assert_allowed(
            "native/include/xnn_transfer/core/model.hpp",
            "#include <array>\n#include <cstdint>",
        )
        self.assert_allowed(
            "native/src/protocol/parser.cpp",
            '#include "protocol/parser.hpp"',
        )

    def test_rejects_private_and_boundary_crossing_includes(self) -> None:
        cases = (
            (
                "native/src/session/session.cpp",
                '#include "xnn_transfer/c_api.h"',
                "public C ABI may only be included",
            ),
            (
                "native/src/session/session.cpp",
                '#include "../storage/private.hpp"',
                "private source path",
            ),
            (
                "native/src/session/session.cpp",
                '#include "native/src/storage/private.hpp"',
                "private source path",
            ),
            (
                "native/src/session/session.cpp",
                "#include <flutter/flutter_embedder.h>",
                "must not include Flutter or Dart",
            ),
            (
                "native/include/xnn_transfer/core/session.hpp",
                "#include <openssl/ssl.h>",
                "must not expose infrastructure",
            ),
            (
                "native/include/xnn_transfer/core/discovery.hpp",
                "#include <utf8proc.h>",
                "must not expose infrastructure",
            ),
            (
                "native/include/xnn_transfer/core/storage.hpp",
                "#include <filesystem>",
                "must not expose infrastructure",
            ),
        )
        for path, source, expected in cases:
            with self.subTest(path=path):
                self.assert_rejected(path, source, expected)


class CMakeDependencyTests(unittest.TestCase):
    def scan(self, source: str) -> list[architecture.Violation]:
        return architecture.scan_cmake_text(
            PurePosixPath("native/src/session/CMakeLists.txt"),
            source,
        )

    def test_allows_reviewed_runtime_target_links(self) -> None:
        source = """
target_link_libraries(
  xnn_transfer_session
  PRIVATE
    xnn_transfer_identity
    xnn_transfer_protocol
    xnn_transfer_tls
)
"""
        self.assertEqual(self.scan(source), [])

    def test_allows_public_headers_and_private_source_directories(self) -> None:
        source = """
target_include_directories(
  xnn_transfer_session
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>"
    "$<INSTALL_INTERFACE:include>"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
"""
        self.assertEqual(self.scan(source), [])

    def test_rejects_public_native_source_directories(self) -> None:
        cases = (
            """
target_include_directories(
  xnn_transfer_session
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>"
)
""",
            """
target_include_directories(
  xnn_transfer_session
  INTERFACE "${PROJECT_SOURCE_DIR}/native/src/protocol"
)
""",
        )
        for source in cases:
            with self.subTest(source=source):
                self.assertTrue(
                    any(
                        "must not expose native/src" in message
                        for message in messages(self.scan(source))
                    )
                )

    def test_ignores_test_target_links(self) -> None:
        source = """
target_link_libraries(
  xnn_transfer_session_tests
  PRIVATE xnn_transfer_session xnn_transfer_storage
)
"""
        self.assertEqual(self.scan(source), [])

    def test_rejects_reverse_and_unknown_runtime_target_links(self) -> None:
        cases = (
            (
                "target_link_libraries(xnn_transfer_protocol "
                "PRIVATE xnn_transfer_session)",
                "xnn_transfer_protocol must not link xnn_transfer_session",
            ),
            (
                "target_link_libraries(xnn_transfer_storage "
                "PRIVATE xnn_transfer_core)",
                "xnn_transfer_storage must not link xnn_transfer_core",
            ),
            (
                "target_link_libraries(xnn_transfer_session "
                "PRIVATE xnn_transfer_unreviewed)",
                "xnn_transfer_session must not link xnn_transfer_unreviewed",
            ),
        )
        for source, expected in cases:
            with self.subTest(source=source):
                self.assertIn(expected, messages(self.scan(source)))


class ModuleInventoryTests(unittest.TestCase):
    def module(self, replacement: str | None = None) -> architecture.Module:
        return architecture.Module(
            id="tls",
            target="xnn_transfer_tls",
            definition=PurePosixPath(
                "native/src/security/tls/CMakeLists.txt"
            ),
            concrete_type="STATIC",
            owned_roots=(
                PurePosixPath("native/src/security/tls"),
            ),
            allowed_project_dependencies=frozenset(),
            placeholder_until=replacement,
        )

    def repository(
        self, cmake: str, state: str = "ready"
    ) -> tuple[tempfile.TemporaryDirectory[str], Path, Path]:
        temporary = tempfile.TemporaryDirectory()
        root = Path(temporary.name)
        cmake_path = (
            root / "native" / "src" / "security" / "tls" / "CMakeLists.txt"
        )
        cmake_path.parent.mkdir(parents=True)
        cmake_path.write_text(cmake, encoding="utf-8")
        record_path = root / ".agents" / "records" / "XT-023.json"
        record_path.parent.mkdir(parents=True)
        record_path.write_text(
            json.dumps({"id": "XT-023", "state": state}) + "\n",
            encoding="utf-8",
        )
        return temporary, root, cmake_path

    def test_placeholder_must_be_replaced_after_task_starts(self) -> None:
        temporary, root, cmake_path = self.repository(
            "add_library(xnn_transfer_tls INTERFACE)\n",
            state="in_progress",
        )
        with temporary:
            violations = architecture.validate_module_inventory(
                root,
                [self.module("XT-023")],
                [cmake_path],
                architecture.load_records(root),
            )
        self.assertIn(
            "XT-023 started but xnn_transfer_tls is still an INTERFACE placeholder",
            messages(violations),
        )

    def test_concrete_replacement_is_accepted_in_progress(self) -> None:
        temporary, root, cmake_path = self.repository(
            "add_library(xnn_transfer_tls STATIC tls.cpp)\n",
            state="in_progress",
        )
        with temporary:
            violations = architecture.validate_module_inventory(
                root,
                [self.module("XT-023")],
                [cmake_path],
                architecture.load_records(root),
            )
        self.assertEqual(violations, [])

    def test_rejects_duplicate_and_undeclared_providers(self) -> None:
        temporary, root, cmake_path = self.repository(
            "add_library(xnn_transfer_tls STATIC tls.cpp)\n"
            "add_library(xnn_transfer_tls STATIC duplicate.cpp)\n"
            "add_library(xnn_transfer_tls_v2 STATIC v2.cpp)\n",
            state="in_progress",
        )
        with temporary:
            violations = architecture.validate_module_inventory(
                root,
                [self.module("XT-023")],
                [cmake_path],
                architecture.load_records(root),
            )
        rendered = messages(violations)
        self.assertTrue(
            any("exactly one add_library definition" in item for item in rendered)
        )
        self.assertTrue(
            any("not declared in the module inventory" in item for item in rendered)
        )


class TemporaryLeaseTests(unittest.TestCase):
    def repository(
        self,
        source: str,
        *,
        removal_state: str = "ready",
        retires: bool = False,
    ) -> tuple[tempfile.TemporaryDirectory[str], Path, dict[str, dict]]:
        temporary = tempfile.TemporaryDirectory()
        root = Path(temporary.name)
        source_path = root / "native" / "src" / "session" / "shim.cpp"
        source_path.parent.mkdir(parents=True)
        source_path.write_text(source, encoding="utf-8")
        records = {
            "XT-100": {
                "id": "XT-100",
                "state": "done",
                "architecture_change": {
                    "temporary_leases": [
                        {
                            "id": "session-shim",
                            "path": "native/src/session/shim.cpp",
                            "remove_by_task": "XT-101",
                            "reason": "Keep one reviewed compatibility shim.",
                        }
                    ],
                    "retires_leases": [],
                },
            },
            "XT-101": {
                "id": "XT-101",
                "state": removal_state,
                "architecture_change": {
                    "temporary_leases": [],
                    "retires_leases": (
                        ["session-shim"] if retires else []
                    ),
                },
            },
        }
        return temporary, root, records

    def test_accepts_active_registered_lease(self) -> None:
        temporary, root, records = self.repository(
            "// XNN-TEMPORARY(session-shim)\n"
        )
        with temporary:
            self.assertEqual(
                architecture.validate_temporary_leases(root, records),
                [],
            )

    def test_rejects_unregistered_marker_and_unleased_todo(self) -> None:
        temporary, root, records = self.repository(
            "// XNN-TEMPORARY(other-shim)\n// TODO remove fallback\n"
        )
        with temporary:
            violations = architecture.validate_temporary_leases(root, records)
        rendered = messages(violations)
        self.assertTrue(
            any("has no registered lease" in item for item in rendered)
        )
        self.assertTrue(
            any("TODO/FIXME" in item for item in rendered)
        )

    def test_rejects_marker_after_removal_task_starts(self) -> None:
        temporary, root, records = self.repository(
            "// XNN-TEMPORARY(session-shim)\n",
            removal_state="done",
            retires=True,
        )
        with temporary:
            violations = architecture.validate_temporary_leases(root, records)
        self.assertTrue(
            any("survived removal task" in item for item in messages(violations))
        )

    def test_accepts_declared_lease_retirement(self) -> None:
        temporary, root, records = self.repository(
            "",
            removal_state="in_progress",
            retires=True,
        )
        with temporary:
            self.assertEqual(
                architecture.validate_temporary_leases(root, records),
                [],
            )

    def test_rejects_unknown_lease_retirement(self) -> None:
        temporary, root, records = self.repository(
            "// XNN-TEMPORARY(session-shim)\n"
        )
        records["XT-101"]["architecture_change"]["retires_leases"] = [
            "unknown-shim"
        ]
        with temporary:
            violations = architecture.validate_temporary_leases(root, records)
        self.assertTrue(
            any(
                "retires unknown temporary lease" in item
                for item in messages(violations)
            )
        )


if __name__ == "__main__":
    unittest.main()
