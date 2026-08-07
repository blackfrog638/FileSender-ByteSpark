#!/usr/bin/env python3

"""Validate the pinned P1 dependency graph without resolving the network."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Optional


VCPKG_COMMIT = "17f35ad2418007a895ced8a4cece4ab34068a58d"
EXPECTED_VERSIONS = {
    "asio": "1.38.2",
    "libsecret": "0.21.7",
    "openssl": "3.5.7",
    "utf8proc": "2.11.3",
}
PLATFORM_EXPRESSIONS = {"libsecret": "linux"}
LIBSECRET_PROVENANCE = {
    "url": (
        "https://download.gnome.org/sources/libsecret/0.21/"
        "libsecret-0.21.7.tar.xz"
    ),
    "sha256": (
        "6b452e4750590a2b5617adc40026f28d2f4903de15f1250e1d1c40bfd68ed55e"
    ),
    "sha512": (
        "f5ee1244338ba324ae403096ddd7357899f55fa9f961d2473515ac924164fe9b3"
        "3f87e39eea2a30b99fc32f2300c0e626d20c98509dbbcadb2c99628a1caa0e4"
    ),
    "license": "third_party/licenses/libsecret-0.21.7.txt",
    "license_sha256": (
        "a1a33180d02960ab1c5de36cf20b1a2f0fe9888d83826ad263da5db52f1b183b"
    ),
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
        if name == "libsecret":
            for field, expected in LIBSECRET_PROVENANCE.items():
                require(
                    dependency.get(field) == expected,
                    f"libsecret {field} provenance drifted",
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
        expected_platform = PLATFORM_EXPRESSIONS.get(name)
        if expected_platform is None:
            require(
                "platform" not in item,
                f"{name} must remain available on every supported platform",
            )
        else:
            require(
                item.get("platform") == expected_platform,
                f"{name} platform expression drifted",
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


def cmake_path(path: Path) -> str:
    return path.resolve().as_posix().replace('"', '\\"')


def libsecret_policy_fixture(root: Path) -> str:
    dependency_file = cmake_path(root / "cmake/XnnDependencies.cmake")
    return (
        r"""
cmake_minimum_required(VERSION 3.24)
project(XnnLibsecretPolicy NONE)
enable_testing()

set(CMAKE_SYSTEM_NAME "Linux")
set(CMAKE_BUILD_TYPE "Debug")
set(VCPKG_INSTALLED_DIR "${CMAKE_BINARY_DIR}/installed")
set(VCPKG_TARGET_TRIPLET "xnn-x64-linux-static")
set(XNN_TRANSFER_BUILD_TESTS ON)
set(_fixture_root "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
file(
  MAKE_DIRECTORY
  "${_fixture_root}/debug/lib/pkgconfig"
  "${_fixture_root}/lib/pkgconfig"
  "${_fixture_root}/share/pkgconfig"
  "${_fixture_root}/include"
  "${CMAKE_BINARY_DIR}/escaped/include"
)
file(WRITE "${_fixture_root}/debug/lib/libsecret-1.a" "")
file(WRITE "${_fixture_root}/debug/lib/libsecret-1.so" "")
set(
  _fixture_pkg_config_libdir
  "${_fixture_root}/debug/lib/pkgconfig:"
  "${_fixture_root}/lib/pkgconfig:"
  "${_fixture_root}/share/pkgconfig"
)
string(CONCAT _fixture_pkg_config_libdir ${_fixture_pkg_config_libdir})

set(ENV{PKG_CONFIG_LIBDIR} "host-libdir")
set(ENV{PKG_CONFIG_PATH} "host-path")
set(OPENSSL_VERSION "3.5.7")
set(utf8proc_VERSION "2.11.3")
add_library(asio::asio INTERFACE IMPORTED)
add_library(OpenSSL::SSL INTERFACE IMPORTED)
add_library(OpenSSL::Crypto INTERFACE IMPORTED)
add_library(utf8proc::utf8proc INTERFACE IMPORTED)

function(find_package package)
  set(_allowed_packages asio OpenSSL utf8proc PkgConfig)
  if(NOT "${package}" IN_LIST _allowed_packages)
    message(FATAL_ERROR "unexpected package resolution: ${package}")
  endif()
  get_property(_packages GLOBAL PROPERTY XNN_FIND_PACKAGES)
  list(APPEND _packages "${package}")
  set_property(GLOBAL PROPERTY XNN_FIND_PACKAGES "${_packages}")
endfunction()

function(pkg_check_modules prefix)
  get_property(_calls GLOBAL PROPERTY XNN_PKG_CONFIG_CALLS)
  if("${_calls}" STREQUAL "")
    set(_calls 0)
  endif()
  math(EXPR _calls "${_calls} + 1")
  set_property(GLOBAL PROPERTY XNN_PKG_CONFIG_CALLS "${_calls}")
  if(NOT "${prefix}" STREQUAL "Libsecret" OR
     NOT "${ARGN}" STREQUAL
         "REQUIRED;NO_CMAKE_PATH;NO_CMAKE_ENVIRONMENT_PATH;libsecret-1=0.21.7")
    message(FATAL_ERROR "libsecret pkg-config request drifted: ${prefix};${ARGN}")
  endif()
  if(NOT "$ENV{PKG_CONFIG_LIBDIR}" STREQUAL
         "${_fixture_pkg_config_libdir}" OR
     NOT "$ENV{PKG_CONFIG_PATH}" STREQUAL "")
    message(FATAL_ERROR "libsecret pkg-config environment was not isolated")
  endif()

  if(XNN_FIXTURE_ESCAPE_PATH)
    set(_include_directory "${CMAKE_BINARY_DIR}/escaped/include")
  else()
    set(_include_directory "${_fixture_root}/include")
  endif()
  set(Libsecret_VERSION "0.21.7" PARENT_SCOPE)
  set(Libsecret_STATIC_INCLUDE_DIRS "${_include_directory}" PARENT_SCOPE)
  set(
    Libsecret_STATIC_LIBRARY_DIRS
    "${_fixture_root}/debug/lib;${_fixture_root}/lib"
    PARENT_SCOPE
  )
  if(XNN_FIXTURE_DYNAMIC_ARCHIVE)
    set(
      Libsecret_STATIC_LIBRARIES
      "${_fixture_root}/debug/lib/libsecret-1.so"
      PARENT_SCOPE
    )
  else()
    set(Libsecret_STATIC_LIBRARIES "secret-1;pthread" PARENT_SCOPE)
  endif()
  set(Libsecret_STATIC_CFLAGS_OTHER "" PARENT_SCOPE)
  set(Libsecret_STATIC_LDFLAGS_OTHER "" PARENT_SCOPE)
endfunction()

function(find_library output)
  set(_arguments "${ARGN}")
  list(FIND _arguments "secret-1" _name_index)
  list(FIND _arguments "${_fixture_root}/debug/lib" _path_index)
  list(FIND _arguments "NO_DEFAULT_PATH" _no_default_index)
  if(NOT "${output}" STREQUAL "_xnn_resolved_library" OR
     _name_index EQUAL -1 OR
     _path_index EQUAL -1 OR
     _no_default_index EQUAL -1)
    message(FATAL_ERROR "libsecret archive lookup permits a host fallback")
  endif()
  set(
    "${output}"
    "${_fixture_root}/debug/lib/libsecret-1.a"
    PARENT_SCOPE
  )
endfunction()

function(add_executable target)
  if("${target}" STREQUAL "xnn_transfer_dependency_probe")
    set_property(GLOBAL PROPERTY XNN_PROBE_ADDED TRUE)
  endif()
endfunction()

function(target_compile_features)
endfunction()

function(target_compile_definitions target)
  set(_arguments "${ARGN}")
  list(FIND _arguments "XNN_TRANSFER_HAS_LIBSECRET=1" _definition_index)
  if("${target}" STREQUAL "xnn_transfer_dependency_probe" AND
     NOT _definition_index EQUAL -1)
    set_property(GLOBAL PROPERTY XNN_PROBE_DEFINED TRUE)
  endif()
endfunction()

function(target_link_libraries target)
  set(_arguments "${ARGN}")
  if("${target}" STREQUAL "xnn_libsecret")
    list(FIND _arguments
         "${_fixture_root}/debug/lib/libsecret-1.a" _archive_index)
    if(NOT _archive_index EQUAL -1)
      set_property(GLOBAL PROPERTY XNN_ARCHIVE_LINKED TRUE)
    endif()
  elseif("${target}" STREQUAL "xnn_transfer_dependency_probe")
    list(FIND _arguments "XnnDependencies::libsecret" _target_index)
    if(NOT _target_index EQUAL -1)
      set_property(GLOBAL PROPERTY XNN_PROBE_LINKED TRUE)
    endif()
  endif()
endfunction()

function(add_test)
endfunction()

function(set_tests_properties)
  set(_arguments "${ARGN}")
  list(FIND _arguments "PASS_REGULAR_EXPRESSION" _property_index)
  if(NOT _property_index EQUAL -1)
    math(EXPR _value_index "${_property_index} + 1")
    list(GET _arguments "${_value_index}" _value)
    if(NOT "${_value}" STREQUAL "libsecret 0\\.21\\.7")
      message(FATAL_ERROR "libsecret probe pass expression drifted")
    endif()
    set_property(GLOBAL PROPERTY XNN_PASS_EXPRESSION TRUE)
  endif()
endfunction()

include("@DEPENDENCY_FILE@")

if(NOT "$ENV{PKG_CONFIG_LIBDIR}" STREQUAL "host-libdir" OR
   NOT "$ENV{PKG_CONFIG_PATH}" STREQUAL "host-path")
  message(FATAL_ERROR "libsecret pkg-config environment was not restored")
endif()
get_property(_pkg_calls GLOBAL PROPERTY XNN_PKG_CONFIG_CALLS)
if(NOT "${_pkg_calls}" STREQUAL "1")
  message(FATAL_ERROR "libsecret resolver did not execute exactly once")
endif()
get_property(_find_packages GLOBAL PROPERTY XNN_FIND_PACKAGES)
if(NOT "${_find_packages}" STREQUAL "asio;OpenSSL;utf8proc;PkgConfig")
  message(FATAL_ERROR "dependency package resolution drifted: ${_find_packages}")
endif()
if(NOT XNN_FIXTURE_ESCAPE_PATH AND NOT XNN_FIXTURE_DYNAMIC_ARCHIVE)
  foreach(
    _required_property
    IN ITEMS
      XNN_PROBE_ADDED
      XNN_PROBE_DEFINED
      XNN_ARCHIVE_LINKED
      XNN_PROBE_LINKED
      XNN_PASS_EXPRESSION
  )
    get_property(_value GLOBAL PROPERTY "${_required_property}")
    if(NOT _value)
      message(FATAL_ERROR "missing executed policy: ${_required_property}")
    endif()
  endforeach()
endif()
"""
    ).replace("@DEPENDENCY_FILE@", dependency_file)


def validate_libsecret_resolution(root: Path) -> None:
    source = (root / "cmake/XnnDependencies.cmake").read_text(encoding="utf-8")
    active = re.sub(r"(?m)#.*$", "", source)
    commands = set(
        re.findall(
            r"(?m)^[ \t]*([A-Za-z_][A-Za-z0-9_]*)[ \t]*\(",
            active,
        )
    )
    cmake_313_commands = {
        "add_executable",
        "add_library",
        "add_test",
        "else",
        "elseif",
        "endif",
        "endforeach",
        "endfunction",
        "file",
        "find_library",
        "find_package",
        "foreach",
        "function",
        "get_filename_component",
        "get_target_property",
        "if",
        "include_guard",
        "list",
        "message",
        "pkg_check_modules",
        "set",
        "set_tests_properties",
        "target_compile_definitions",
        "target_compile_features",
        "target_compile_options",
        "target_include_directories",
        "target_link_directories",
        "target_link_libraries",
        "target_link_options",
        "unset",
        "xnn_require_vcpkg_dependency_path",
    }
    unsupported_commands = commands - cmake_313_commands
    require(
        not unsupported_commands,
        "Linux desktop dependency resolution requires CMake newer than 3.13: "
        + ", ".join(sorted(unsupported_commands)),
    )
    for command, allowed_subcommands in (
        ("file", {"RELATIVE_PATH"}),
        ("list", {"APPEND", "JOIN"}),
    ):
        subcommands = set(
            re.findall(
                rf"(?is)\b{command}\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)",
                active,
            )
        )
        require(
            subcommands <= allowed_subcommands,
            "Linux desktop dependency resolution uses an unreviewed "
            f"{command} subcommand: "
            + ", ".join(sorted(subcommands - allowed_subcommands)),
        )
    filename_component_modes = {
        re.sub(r"\s+", " ", body).strip().split()[-1]
        for body in re.findall(
            r"(?is)\bget_filename_component\s*\((.*?)\)",
            active,
        )
    }
    require(
        filename_component_modes <= {"REALPATH"},
        "Linux desktop dependency resolution uses an unreviewed "
        "get_filename_component mode: "
        + ", ".join(sorted(filename_component_modes - {"REALPATH"})),
    )

    with tempfile.TemporaryDirectory(
        prefix="xnn-transfer-libsecret-policy-"
    ) as temporary:
        fixture_root = Path(temporary)
        (fixture_root / "CMakeLists.txt").write_text(
            libsecret_policy_fixture(root),
            encoding="utf-8",
        )
        modes = (
            ("accepted", (), True),
            ("escaped", ("-DXNN_FIXTURE_ESCAPE_PATH=ON",), False),
            ("dynamic", ("-DXNN_FIXTURE_DYNAMIC_ARCHIVE=ON",), False),
        )
        for name, arguments, must_succeed in modes:
            result = subprocess.run(
                [
                    "cmake",
                    "-S",
                    str(fixture_root),
                    "-B",
                    str(fixture_root / f"build-{name}"),
                    *arguments,
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            output = result.stdout + result.stderr
            require(
                (result.returncode == 0) is must_succeed,
                "Linux libsecret system fallback, static linkage, or target "
                "consumption CMake fixture "
                f"{name} {'failed' if must_succeed else 'was accepted'}:\n"
                f"{output}",
            )


def active_cpp_source(source: str, defined_macros: set[str]) -> str:
    without_comments = re.sub(r"/\*.*?\*/", "", source, flags=re.DOTALL)
    without_comments = re.sub(r"//.*$", "", without_comments, flags=re.MULTILINE)
    active = True
    conditions: list[tuple[bool, bool]] = []
    result: list[str] = []

    def evaluate(expression: str) -> bool:
        normalized = re.sub(r"\s+", "", expression)
        if normalized in ("0", "1"):
            return normalized == "1"
        match = re.fullmatch(r"defined\(?([A-Za-z_]\w*)\)?", normalized)
        if match is not None:
            return match.group(1) in defined_macros
        match = re.fullmatch(r"!defined\(?([A-Za-z_]\w*)\)?", normalized)
        if match is not None:
            return match.group(1) not in defined_macros
        raise DependencyError(
            f"unsupported dependency probe preprocessor condition: {expression}"
        )

    for line in without_comments.splitlines():
        directive = re.match(r"\s*#\s*(\w+)(?:\s+(.*?))?\s*$", line)
        if directive is None:
            if active:
                result.append(line)
            continue
        command = directive.group(1)
        argument = directive.group(2) or ""
        if command in ("if", "ifdef", "ifndef"):
            if command == "if":
                condition = evaluate(argument)
            elif command == "ifdef":
                condition = argument.strip() in defined_macros
            else:
                condition = argument.strip() not in defined_macros
            conditions.append((active, condition))
            active = active and condition
        elif command == "else":
            require(bool(conditions), "dependency probe has an unmatched #else")
            parent, condition = conditions[-1]
            conditions[-1] = (parent, not condition)
            active = parent and not condition
        elif command == "endif":
            require(bool(conditions), "dependency probe has an unmatched #endif")
            parent, _ = conditions.pop()
            active = parent
        elif command == "elif":
            raise DependencyError(
                "dependency probe must not use #elif around pinned checks"
            )
        elif active and command == "include":
            result.append(line)
        elif active:
            raise DependencyError(
                "dependency probe uses an unsupported active preprocessor "
                f"directive: #{command}"
            )
    require(not conditions, "dependency probe has an unmatched preprocessor block")
    return "\n".join(result)


def strip_cpp_literals(source: str) -> str:
    result: list[str] = []
    index = 0
    while index < len(source):
        if source.startswith('R"', index):
            delimiter_end = source.find("(", index + 2)
            require(
                delimiter_end != -1,
                "dependency probe has an unterminated raw string delimiter",
            )
            delimiter = source[index + 2 : delimiter_end]
            terminator = ")" + delimiter + '"'
            literal_end = source.find(terminator, delimiter_end + 1)
            require(
                literal_end != -1,
                "dependency probe has an unterminated raw string",
            )
            result.append('""')
            result.extend(
                "\n"
                for character in source[index : literal_end + len(terminator)]
                if character == "\n"
            )
            index = literal_end + len(terminator)
            continue

        quote = source[index]
        is_digit_separator = (
            quote == "'"
            and index > 0
            and index + 1 < len(source)
            and source[index - 1].isdigit()
            and source[index + 1].isdigit()
        )
        if quote not in ('"', "'") or is_digit_separator:
            result.append(quote)
            index += 1
            continue

        result.append(quote * 2)
        index += 1
        escaped = False
        while index < len(source):
            character = source[index]
            if character == "\n":
                result.append("\n")
            if not escaped and character == quote:
                index += 1
                break
            if not escaped and character == "\\":
                escaped = True
            else:
                escaped = False
            index += 1
        else:
            raise DependencyError("dependency probe has an unterminated literal")
    return "".join(result)


def cpp_main_body(source: str) -> str:
    without_literals = strip_cpp_literals(source)
    main = re.search(r"\bint\s+main\s*\(\s*\)\s*\{", without_literals)
    require(main is not None, "dependency probe is missing main")
    depth = 1
    for index in range(main.end(), len(without_literals)):
        if without_literals[index] == "{":
            depth += 1
        elif without_literals[index] == "}":
            depth -= 1
            if depth == 0:
                return without_literals[main.end() : index]
    raise DependencyError("dependency probe main has unmatched braces")


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
    validate_libsecret_resolution(root)
    probe = active_cpp_source(
        (root / "cmake/dependencies/dependency_probe.cpp").read_text(
            encoding="utf-8"
        ),
        {"XNN_TRANSFER_HAS_LIBSECRET"},
    )
    normalized_probe = re.sub(r"\s+", " ", probe)
    for fragment in (
        "#include <libsecret/secret.h>",
        "static_assert(SECRET_MAJOR_VERSION == 0);",
        "static_assert(SECRET_MINOR_VERSION == 21);",
        "static_assert(SECRET_MICRO_VERSION == 7);",
    ):
        require(
            fragment in normalized_probe,
            "Linux dependency probe does not prove libsecret version headers",
        )
    normalized_main = re.sub(r"\s+", " ", cpp_main_body(probe))
    for fragment in (
        "const GType libsecret_type = secret_service_get_type();",
        "if (libsecret_type == G_TYPE_INVALID)",
        "<< SECRET_MAJOR_VERSION",
        "<< SECRET_MINOR_VERSION",
        "<< SECRET_MICRO_VERSION",
        "<< libsecret_type;",
    ):
        require(
            fragment in normalized_main,
            "Linux dependency probe does not prove libsecret symbol consumption",
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
    port_status = subprocess.run(
        [
            "git",
            "-C",
            str(vcpkg_root),
            "status",
            "--porcelain=v1",
            "--untracked-files=all",
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    require(
        not port_status.stdout.strip(),
        "pinned vcpkg checkout has local modifications",
    )
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
    libsecret = read_json(vcpkg_root / "ports/libsecret/vcpkg.json")
    require(
        libsecret.get("version") == EXPECTED_VERSIONS["libsecret"],
        "pinned registry does not provide libsecret 0.21.7",
    )
    require(
        libsecret.get("license") == "LGPL-2.1-or-later",
        "pinned registry libsecret license drifted",
    )
    require(
        libsecret.get("supports") == "!android & !ios & !osx & !windows",
        "pinned registry libsecret platform support drifted",
    )
    libsecret_portfile = (
        vcpkg_root / "ports/libsecret/portfile.cmake"
    ).read_text(encoding="utf-8")
    require(
        dependencies["libsecret"]["sha512"] in libsecret_portfile,
        "pinned registry libsecret SHA-512 drifted",
    )
    require(
        (
            '"https://download.gnome.org/sources/${PORT}/'
            '${VERSION_MAJOR_MINOR}/${PORT}-${VERSION}.tar.xz"'
        )
        in libsecret_portfile
        and "vcpkg_download_distfile" in libsecret_portfile,
        "pinned registry libsecret source URL drifted",
    )


def validate_linux_libsecret_link(root: Path, vcpkg_root: Path) -> None:
    if not sys.platform.startswith("linux"):
        return

    with tempfile.TemporaryDirectory(
        prefix="xnn-transfer-libsecret-link-"
    ) as temporary:
        probe_root = Path(temporary)
        (probe_root / "CMakeLists.txt").write_text(
            (
                """
cmake_minimum_required(VERSION 3.13)
include("@ROOT@/cmake/XnnVcpkg.cmake")
project(XnnGeneratedLibsecretProbe LANGUAGES CXX)
set(XNN_TRANSFER_BUILD_TESTS OFF)
include("@ROOT@/cmake/XnnDependencies.cmake")
add_executable(xnn_generated_libsecret_probe probe.cpp)
target_compile_features(xnn_generated_libsecret_probe PRIVATE cxx_std_20)
target_link_libraries(
  xnn_generated_libsecret_probe
  PRIVATE
    XnnDependencies::libsecret
)
"""
            ).replace("@ROOT@", cmake_path(root)),
            encoding="utf-8",
        )
        (probe_root / "probe.cpp").write_text(
            """
#include <libsecret/secret.h>

#include <iostream>

static_assert(SECRET_MAJOR_VERSION == 0);
static_assert(SECRET_MINOR_VERSION == 21);
static_assert(SECRET_MICRO_VERSION == 7);

int main() {
  const GType service_type = secret_service_get_type();
  if (service_type == G_TYPE_INVALID) {
    return 1;
  }
  std::cout << "libsecret " << SECRET_MAJOR_VERSION << '.'
            << SECRET_MINOR_VERSION << '.' << SECRET_MICRO_VERSION << " type "
            << service_type << '\\n';
  return 0;
}
""",
            encoding="utf-8",
        )
        build_root = probe_root / "build"
        commands = (
            [
                "cmake",
                "-S",
                str(probe_root),
                "-B",
                str(build_root),
                f"-DXNN_TRANSFER_VCPKG_ROOT={vcpkg_root}",
            ],
            ["cmake", "--build", str(build_root)],
        )
        for command in commands:
            result = subprocess.run(
                command,
                check=False,
                capture_output=True,
                text=True,
            )
            require(
                result.returncode == 0,
                "generated Linux libsecret link probe failed:\n"
                + result.stdout
                + result.stderr,
            )
        executables = [
            path
            for path in build_root.rglob("xnn_generated_libsecret_probe")
            if path.is_file() and path.stat().st_mode & 0o111
        ]
        require(
            len(executables) == 1,
            "generated Linux libsecret probe executable is missing",
        )
        result = subprocess.run(
            [str(executables[0])],
            check=False,
            capture_output=True,
            text=True,
        )
        require(
            result.returncode == 0
            and re.fullmatch(
                r"libsecret 0\.21\.7 type [1-9][0-9]*\n",
                result.stdout,
            )
            is not None,
            "generated Linux libsecret probe did not consume the real symbol",
        )


def validate(root: Path, vcpkg_root: Optional[Path] = None) -> None:
    dependencies = validate_manifest(root)
    validate_overlays(root, dependencies)
    validate_triplets(root)
    validate_build_entrypoints(root)
    if vcpkg_root is not None:
        validate_vcpkg_checkout(root, vcpkg_root, dependencies)
        validate_linux_libsecret_link(root, vcpkg_root)


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
