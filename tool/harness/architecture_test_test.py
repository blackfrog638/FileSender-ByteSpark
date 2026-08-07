#!/usr/bin/env python3

"""Focused tests for the mechanical architecture dependency gate."""

from __future__ import annotations

import unittest
from pathlib import PurePosixPath

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


if __name__ == "__main__":
    unittest.main()
