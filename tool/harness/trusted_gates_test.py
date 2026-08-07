#!/usr/bin/env python3

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from trusted_gates import GateRegistryError, load_gate_registry


class TrustedGateRegistryTests(unittest.TestCase):
    def load(self, source: str) -> dict[str, str]:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "manifest.yaml"
            path.write_text(source, encoding="utf-8")
            return load_gate_registry(path)

    def test_loads_commands_section(self) -> None:
        self.assertEqual(
            self.load(
                """schema_version: 1
commands:
  verify: make verify
  native_test: make native-test
workstreams:
  integration:
"""
            ),
            {
                "verify": "make verify",
                "native_test": "make native-test",
            },
        )

    def test_requires_verify_gate(self) -> None:
        with self.assertRaisesRegex(
            GateRegistryError, "must define verify"
        ):
            self.load(
                """commands:
  native_test: make native-test
"""
            )

    def test_rejects_malformed_entry(self) -> None:
        with self.assertRaisesRegex(GateRegistryError, "invalid command entry"):
            self.load(
                """commands:
  verify:
"""
            )

    def test_rejects_duplicate_commands(self) -> None:
        with self.assertRaisesRegex(
            GateRegistryError, "commands must be unique"
        ):
            self.load(
                """commands:
  verify: make verify
  duplicate: make verify
"""
            )


if __name__ == "__main__":
    unittest.main()
