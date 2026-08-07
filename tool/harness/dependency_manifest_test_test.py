#!/usr/bin/env python3

"""Negative fixtures for pinned dependency manifest validation."""

from __future__ import annotations

import json
import shutil
import tempfile
import unittest
from pathlib import Path
from unittest import mock

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

    def test_rejects_dirty_vcpkg_version_database(self) -> None:
        vcpkg_root = self.root / "fake-vcpkg"
        (vcpkg_root / ".git").mkdir(parents=True)
        dependencies = dependency.validate_manifest(self.root)
        with mock.patch.object(
            dependency.subprocess,
            "run",
            side_effect=(
                mock.Mock(stdout=dependency.VCPKG_COMMIT + "\n"),
                mock.Mock(stdout=" M versions/l-/libsecret.json\n"),
            ),
        ):
            with self.assertRaisesRegex(
                dependency.DependencyError,
                "local modifications",
            ):
                dependency.validate_vcpkg_checkout(
                    self.root,
                    vcpkg_root,
                    dependencies,
                )

    def test_rejects_source_hash_drift(self) -> None:
        self.mutate_json(
            "cmake/dependencies/versions.json",
            lambda value: value["dependencies"]["openssl"].update(
                {"sha512": "0" * 128}
            ),
        )
        with self.assertRaisesRegex(dependency.DependencyError, "OpenSSL|openssl"):
            dependency.validate(self.root)

    def test_rejects_libsecret_version_drift(self) -> None:
        self.mutate_json(
            "cmake/dependencies/versions.json",
            lambda value: value["dependencies"]["libsecret"].update(
                {"version": "0.21.6"}
            ),
        )
        with self.assertRaisesRegex(dependency.DependencyError, "libsecret version"):
            dependency.validate(self.root)

    def test_rejects_libsecret_source_url_drift(self) -> None:
        self.mutate_json(
            "cmake/dependencies/versions.json",
            lambda value: value["dependencies"]["libsecret"].update(
                {"url": "https://attacker.invalid/libsecret-0.21.7.tar.xz"}
            ),
        )
        with self.assertRaisesRegex(dependency.DependencyError, "provenance"):
            dependency.validate(self.root)

    def test_rejects_unscoped_libsecret(self) -> None:
        def remove_platform(value) -> None:
            item = next(
                entry
                for entry in value["dependencies"]
                if entry["name"] == "libsecret"
            )
            item.pop("platform")

        self.mutate_json("vcpkg.json", remove_platform)
        with self.assertRaisesRegex(dependency.DependencyError, "platform expression"):
            dependency.validate(self.root)

    def test_rejects_license_drift(self) -> None:
        path = self.root / "third_party/licenses/asio-1.38.2.txt"
        path.write_text("changed\n", encoding="utf-8")
        with self.assertRaisesRegex(dependency.DependencyError, "license hash"):
            dependency.validate(self.root)

    def test_rejects_libsecret_license_drift(self) -> None:
        path = self.root / "third_party/licenses/libsecret-0.21.7.txt"
        path.write_text("changed\n", encoding="utf-8")
        with self.assertRaisesRegex(dependency.DependencyError, "license hash"):
            dependency.validate(self.root)

    def test_rejects_rehashed_libsecret_license_drift(self) -> None:
        path = self.root / "third_party/licenses/libsecret-0.21.7.txt"
        path.write_text("changed\n", encoding="utf-8")
        self.mutate_json(
            "cmake/dependencies/versions.json",
            lambda value: value["dependencies"]["libsecret"].update(
                {"license_sha256": dependency.sha256(path)}
            ),
        )
        with self.assertRaisesRegex(dependency.DependencyError, "provenance"):
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

    def test_rejects_libsecret_system_fallback(self) -> None:
        path = self.root / "cmake/XnnDependencies.cmake"
        text = path.read_text(encoding="utf-8")
        path.write_text(
            text.replace("PKG_CONFIG_LIBDIR", "UNSCOPED_PKG_CONFIG_PATH"),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(dependency.DependencyError, "system fallback"):
            dependency.validate(self.root)

    def test_rejects_cmake_prefix_pkg_config_fallback(self) -> None:
        path = self.root / "cmake/XnnDependencies.cmake"
        text = path.read_text(encoding="utf-8")
        path.write_text(
            text.replace("    NO_CMAKE_ENVIRONMENT_PATH\n", ""),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(dependency.DependencyError, "system fallback"):
            dependency.validate(self.root)

    def test_rejects_missing_atomic_runtime_link(self) -> None:
        path = self.root / "cmake/XnnDependencies.cmake"
        text = path.read_text(encoding="utf-8")
        path.write_text(
            text.replace(
                "set(_xnn_system_link_libraries atomic dl m pthread rt -pthread)",
                "set(_xnn_system_link_libraries dl m pthread rt -pthread)",
            ),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(dependency.DependencyError, "CMake fixture"):
            dependency.validate(self.root)

    def test_rejects_unreviewed_system_runtime_link(self) -> None:
        path = self.root / "cmake/XnnDependencies.cmake"
        text = path.read_text(encoding="utf-8")
        path.write_text(
            text.replace(
                "set(_xnn_system_link_libraries atomic dl m pthread rt -pthread)",
                "set(_xnn_system_link_libraries "
                "atomic dl m pthread resolv rt -pthread)",
            ),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(dependency.DependencyError, "CMake fixture"):
            dependency.validate(self.root)

    def test_rejects_dependency_command_newer_than_flutter_minimum(self) -> None:
        path = self.root / "cmake/XnnDependencies.cmake"
        text = path.read_text(encoding="utf-8")
        path.write_text(
            text.replace(
                'if(CMAKE_SYSTEM_NAME STREQUAL "Linux")\n',
                'if(CMAKE_SYSTEM_NAME STREQUAL "Linux")\n'
                "  block()\n"
                "  endblock()\n",
            ),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(
            dependency.DependencyError,
            "CMake newer than 3.13",
        ):
            dependency.validate(self.root)

    def test_rejects_variable_expanded_package_fallback(self) -> None:
        path = self.root / "cmake/XnnDependencies.cmake"
        text = path.read_text(encoding="utf-8")
        resolver = (
            "  pkg_check_modules(\n"
            "    Libsecret\n"
            "    REQUIRED\n"
            "    NO_CMAKE_PATH\n"
            "    NO_CMAKE_ENVIRONMENT_PATH\n"
            "    libsecret-1=0.21.7\n"
            "  )\n"
        )
        path.write_text(
            text.replace(
                resolver,
                resolver
                + "  set(_xnn_fallback_package libsecret)\n"
                + "  find_package(${_xnn_fallback_package} REQUIRED)\n",
            ),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(
            dependency.DependencyError,
            "CMake fixture",
        ):
            dependency.validate(self.root)

    def test_rejects_inactive_pkg_config_resolver(self) -> None:
        path = self.root / "cmake/XnnDependencies.cmake"
        text = path.read_text(encoding="utf-8")
        resolver = (
            "  pkg_check_modules(\n"
            "    Libsecret\n"
            "    REQUIRED\n"
            "    NO_CMAKE_PATH\n"
            "    NO_CMAKE_ENVIRONMENT_PATH\n"
            "    libsecret-1=0.21.7\n"
            "  )\n"
        )
        path.write_text(
            text.replace(
                resolver,
                "  if(FALSE)\n" + resolver + "  endif()\n",
            ),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(
            dependency.DependencyError,
            "CMake fixture",
        ):
            dependency.validate(self.root)

    def test_rejects_pkg_config_resolver_in_uncalled_function(self) -> None:
        path = self.root / "cmake/XnnDependencies.cmake"
        text = path.read_text(encoding="utf-8")
        resolver = (
            "  pkg_check_modules(\n"
            "    Libsecret\n"
            "    REQUIRED\n"
            "    NO_CMAKE_PATH\n"
            "    NO_CMAKE_ENVIRONMENT_PATH\n"
            "    libsecret-1=0.21.7\n"
            "  )\n"
        )
        path.write_text(
            text.replace(
                resolver,
                "  function(xnn_inactive_resolver)\n"
                + resolver
                + "  endfunction()\n",
            ),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(
            dependency.DependencyError,
            "CMake fixture",
        ):
            dependency.validate(self.root)

    def test_rejects_inactive_pkg_config_isolation(self) -> None:
        path = self.root / "cmake/XnnDependencies.cmake"
        text = path.read_text(encoding="utf-8")
        isolation = '  set(ENV{PKG_CONFIG_PATH} "")\n'
        path.write_text(
            text.replace(
                isolation,
                "  if(FALSE)\n" + isolation + "  endif()\n",
            ),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(
            dependency.DependencyError,
            "system fallback",
        ):
            dependency.validate(self.root)

    def test_rejects_inactive_vcpkg_path_validation(self) -> None:
        path = self.root / "cmake/XnnDependencies.cmake"
        text = path.read_text(encoding="utf-8")
        validation = (
            "    xnn_require_vcpkg_dependency_path(\n"
            '      "${_xnn_include_directory}"\n'
            '      "libsecret include directory"\n'
            "    )\n"
        )
        path.write_text(
            text.replace(
                validation,
                "    if(FALSE)\n" + validation + "    endif()\n",
            ),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(
            dependency.DependencyError,
            "system fallback",
        ):
            dependency.validate(self.root)

    def test_rejects_inactive_static_archive_validation(self) -> None:
        path = self.root / "cmake/XnnDependencies.cmake"
        text = path.read_text(encoding="utf-8")
        validation = (
            '      if(NOT _xnn_link_library MATCHES "\\\\.a$")\n'
            "        message(\n"
            "          FATAL_ERROR\n"
            '          "libsecret resolved a non-static library: '
            '${_xnn_link_library}"\n'
            "        )\n"
            "      endif()\n"
        )
        path.write_text(
            text.replace(
                validation,
                "      if(FALSE)\n" + validation + "      endif()\n",
            ),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(
            dependency.DependencyError,
            "static linkage",
        ):
            dependency.validate(self.root)

    def test_rejects_reenabled_pkg_config_path(self) -> None:
        path = self.root / "cmake/XnnDependencies.cmake"
        text = path.read_text(encoding="utf-8")
        path.write_text(
            text.replace(
                '  set(ENV{PKG_CONFIG_PATH} "")\n',
                '  set(ENV{PKG_CONFIG_PATH} "")\n'
                '  set(ENV{PKG_CONFIG_PATH} "/usr/lib/pkgconfig")\n',
            ),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(dependency.DependencyError, "fallback"):
            dependency.validate(self.root)

    def test_rejects_unlinked_libsecret_probe(self) -> None:
        path = self.root / "cmake/XnnDependencies.cmake"
        text = path.read_text(encoding="utf-8")
        path.write_text(
            text.replace(
                "    target_link_libraries(\n"
                "      xnn_transfer_dependency_probe\n"
                "      PRIVATE\n"
                "        XnnDependencies::libsecret\n"
                "    )\n",
                "",
            ),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(
            dependency.DependencyError,
            "target consumption",
        ):
            dependency.validate(self.root)

    def test_rejects_vacuous_libsecret_probe(self) -> None:
        path = self.root / "cmake/dependencies/dependency_probe.cpp"
        text = path.read_text(encoding="utf-8")
        path.write_text(
            text.replace("secret_service_get_type()", "G_TYPE_OBJECT"),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(
            dependency.DependencyError,
            "symbol consumption",
        ):
            dependency.validate(self.root)

    def test_rejects_compiled_out_libsecret_probe(self) -> None:
        path = self.root / "cmake/dependencies/dependency_probe.cpp"
        text = path.read_text(encoding="utf-8")
        call = "  const GType libsecret_type = secret_service_get_type();\n"
        path.write_text(
            text.replace(call, "#if 0\n" + call + "#endif\n"),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(
            dependency.DependencyError,
            "symbol consumption",
        ):
            dependency.validate(self.root)

    def test_rejects_libsecret_probe_macro_undef(self) -> None:
        path = self.root / "cmake/dependencies/dependency_probe.cpp"
        text = path.read_text(encoding="utf-8")
        path.write_text(
            text.replace(
                "static_assert(SECRET_MICRO_VERSION == 7);\n",
                "static_assert(SECRET_MICRO_VERSION == 7);\n"
                "#undef XNN_TRANSFER_HAS_LIBSECRET\n",
            ),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(
            dependency.DependencyError,
            "unsupported active preprocessor",
        ):
            dependency.validate(self.root)

    def test_rejects_commented_libsecret_probe(self) -> None:
        path = self.root / "cmake/dependencies/dependency_probe.cpp"
        text = path.read_text(encoding="utf-8")
        call = "  const GType libsecret_type = secret_service_get_type();"
        path.write_text(
            text.replace(call, "  // " + call.strip()),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(
            dependency.DependencyError,
            "symbol consumption",
        ):
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
