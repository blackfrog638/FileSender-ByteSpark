#!/usr/bin/env python3
"""Validate host-independent XnnTransfer v1 manifest fixture objects."""

from __future__ import annotations

import argparse
import json
import sys
import unicodedata
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterator


U32_MAX = (1 << 32) - 1
U64_MAX = (1 << 64) - 1
MAX_ENTRIES = 100_000
MAX_AGGREGATE_PATH_BYTES = 33_554_432
MAX_TOTAL_FILE_BYTES = 17_592_186_044_416
MAX_FILE_BYTES = 17_592_186_044_416
MAX_PATH_BYTES = 1_024
MAX_PATH_COMPONENTS = 32
MAX_COMPONENT_BYTES = 255
MIN_COMMITMENT_BYTES = 16
MAX_COMMITMENT_BYTES = 64

FILE_KIND = 1
DIRECTORY_KIND = 2


class VectorError(Exception):
    """A stable protocol error and fixture-specific failure reason."""

    def __init__(self, error: str, reason: str, detail: str) -> None:
        super().__init__(f"{error}/{reason}: {detail}")
        self.error = error
        self.reason = reason
        self.detail = detail


@dataclass(frozen=True)
class ComparisonProfile:
    normalization: str
    case: str


class PathNode:
    __slots__ = ("children", "kind", "wire_path")

    def __init__(self) -> None:
        self.children: dict[str, "PathNode"] = {}
        self.kind: int | None = None
        self.wire_path: str | None = None


def fail(error: str, reason: str, detail: str) -> None:
    raise VectorError(error, reason, detail)


def require_object(value: Any, name: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        fail("FIXTURE_INVALID", "OBJECT_REQUIRED", f"{name} is not an object")
    return value


def require_integer(
    value: Any,
    name: str,
    maximum: int,
    *,
    error: str = "MALFORMED_MESSAGE",
    reason: str = "INTEGER_OUT_OF_RANGE",
) -> int:
    if type(value) is not int or not 0 <= value <= maximum:
        fail(error, reason, f"{name} is outside unsigned range")
    return value


def decode_hex(value: Any, name: str) -> bytes:
    if not isinstance(value, str):
        fail("FIXTURE_INVALID", "HEX_REQUIRED", f"{name} is not hexadecimal text")
    try:
        return bytes.fromhex(value)
    except ValueError:
        fail("FIXTURE_INVALID", "HEX_REQUIRED", f"{name} is not valid hexadecimal")


def resolve_hex(
    value: dict[str, Any],
    key: str,
    defaults: dict[str, Any],
    default_key: str,
) -> bytes:
    encoded = value.get(key, defaults.get(default_key))
    return decode_hex(encoded, key)


def validate_commitment(
    encoded: bytes,
    *,
    error: str,
    reason: str,
    name: str,
) -> None:
    if not MIN_COMMITMENT_BYTES <= len(encoded) <= MAX_COMMITMENT_BYTES:
        fail(error, reason, f"{name} length is outside v1 limits")


def checked_add_u64(
    current: int,
    increment: int,
    *,
    overflow_reason: str,
) -> int:
    if increment > U64_MAX - current:
        fail("LIMIT_EXCEEDED", overflow_reason, "unsigned 64-bit addition overflow")
    return current + increment


def is_noncharacter(character: str) -> bool:
    value = ord(character)
    return 0xFDD0 <= value <= 0xFDEF or value & 0xFFFF in {0xFFFE, 0xFFFF}


def decode_wire_path(entry: dict[str, Any]) -> tuple[bytes, str]:
    representations = {
        key
        for key in ("relative_path", "relative_path_hex", "relative_path_repeat")
        if key in entry
    }
    if len(representations) != 1:
        fail(
            "FIXTURE_INVALID",
            "PATH_REPRESENTATION",
            "entry must contain exactly one relative path representation",
        )

    if "relative_path" in representations:
        text = entry["relative_path"]
        if not isinstance(text, str):
            fail("FIXTURE_INVALID", "PATH_REPRESENTATION", "path is not text")
        try:
            encoded = text.encode("utf-8", errors="strict")
        except UnicodeEncodeError:
            fail("MALFORMED_MESSAGE", "PATH_INVALID_UTF8", "path has a surrogate")
        return encoded, text

    if "relative_path_repeat" in representations:
        repeat = require_object(entry["relative_path_repeat"], "relative_path_repeat")
        text = repeat.get("text")
        count = repeat.get("count")
        if not isinstance(text, str) or type(count) is not int or count < 0:
            fail(
                "FIXTURE_INVALID",
                "PATH_REPRESENTATION",
                "path repeat requires text and a nonnegative count",
            )
        try:
            repeated = text * count
            return repeated.encode("utf-8", errors="strict"), repeated
        except UnicodeEncodeError:
            fail("MALFORMED_MESSAGE", "PATH_INVALID_UTF8", "path has a surrogate")

    encoded = decode_hex(entry["relative_path_hex"], "relative_path_hex")
    try:
        return encoded, encoded.decode("utf-8", errors="strict")
    except UnicodeDecodeError:
        fail(
            "MALFORMED_MESSAGE",
            "PATH_INVALID_UTF8",
            "path bytes are not shortest-form well-formed UTF-8",
        )


def validate_wire_path(entry: dict[str, Any]) -> tuple[int, str, list[str]]:
    encoded, path = decode_wire_path(entry)
    path_bytes = len(encoded)
    if path_bytes == 0:
        fail("INVALID_MANIFEST", "PATH_EMPTY", "relative path is empty")
    if path_bytes > MAX_PATH_BYTES:
        fail("LIMIT_EXCEEDED", "PATH_BYTES_LIMIT", "relative path exceeds 1024 bytes")

    if path.startswith("//") or path.startswith("\\\\"):
        fail("INVALID_MANIFEST", "PATH_UNC", "UNC path is not representable")
    if path.startswith("/"):
        fail("INVALID_MANIFEST", "PATH_ABSOLUTE", "absolute path is not relative")
    if len(path) >= 2 and path[0].isascii() and path[0].isalpha() and path[1] == ":":
        reason = (
            "PATH_DRIVE_ABSOLUTE"
            if len(path) >= 3 and path[2] in {"/", "\\"}
            else "PATH_DRIVE_QUALIFIED"
        )
        fail("INVALID_MANIFEST", reason, "drive-qualified path is not representable")
    if "\\" in path:
        fail("INVALID_MANIFEST", "PATH_BACKSLASH", "wire separator must be slash")
    if ":" in path:
        fail(
            "INVALID_MANIFEST",
            "PATH_COLON_OR_ADS",
            "colon and alternate data streams are not representable",
        )
    if path.endswith("/"):
        fail(
            "INVALID_MANIFEST",
            "PATH_TRAILING_SEPARATOR",
            "trailing slash creates an empty component",
        )
    if "\x00" in path:
        fail("INVALID_MANIFEST", "PATH_NUL", "path contains U+0000")
    if any(ord(character) <= 0x1F for character in path):
        fail("INVALID_MANIFEST", "PATH_C0_CONTROL", "path contains a C0 control")
    if any(0x80 <= ord(character) <= 0x9F for character in path):
        fail("INVALID_MANIFEST", "PATH_C1_CONTROL", "path contains a C1 control")
    if any(is_noncharacter(character) for character in path):
        fail(
            "INVALID_MANIFEST",
            "PATH_NONCHARACTER",
            "path contains a Unicode noncharacter",
        )
    if unicodedata.normalize("NFC", path) != path:
        fail("INVALID_MANIFEST", "PATH_NOT_NFC", "wire path is not NFC-normalized")

    components = path.split("/")
    if any(component == "" for component in components):
        fail(
            "INVALID_MANIFEST",
            "PATH_EMPTY_COMPONENT",
            "path contains an empty component",
        )
    if any(component == "." for component in components):
        fail("INVALID_MANIFEST", "PATH_DOT_COMPONENT", "path contains dot component")
    if any(component == ".." for component in components):
        fail(
            "INVALID_MANIFEST",
            "PATH_TRAVERSAL",
            "path contains parent traversal component",
        )
    if len(components) > MAX_PATH_COMPONENTS:
        fail(
            "LIMIT_EXCEEDED",
            "PATH_COMPONENT_COUNT_LIMIT",
            "path has more than 32 components",
        )
    if any(
        len(component.encode("utf-8")) > MAX_COMPONENT_BYTES
        for component in components
    ):
        fail(
            "LIMIT_EXCEEDED",
            "PATH_COMPONENT_BYTES_LIMIT",
            "path component exceeds 255 bytes",
        )
    return path_bytes, path, components


def canonical_component(component: str, profile: ComparisonProfile) -> str:
    normalized = unicodedata.normalize(profile.normalization, component)
    if profile.case == "sensitive":
        return normalized
    if profile.case == "ascii_insensitive":
        return normalized.translate(
            str.maketrans(
                "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
                "abcdefghijklmnopqrstuvwxyz",
            )
        )
    fail("FIXTURE_INVALID", "COMPARISON_PROFILE", "unsupported case rule")


def canonical_path(
    components: list[str], profile: ComparisonProfile
) -> list[str]:
    return [canonical_component(component, profile) for component in components]


def insert_path(
    root: PathNode,
    wire_path: str,
    components: list[str],
    kind: int,
) -> None:
    node = root
    for offset, component in enumerate(components):
        if node.kind == FILE_KIND:
            fail(
                "INVALID_MANIFEST",
                "FILE_ANCESTOR_CONFLICT",
                "an existing file is an ancestor of another entry",
            )
        child = node.children.get(component)
        if child is None:
            child = PathNode()
            node.children[component] = child
        node = child
        if offset == len(components) - 1 and node.kind is not None:
            reason = "PATH_DUPLICATE" if node.wire_path == wire_path else "PATH_COLLISION"
            fail(
                "INVALID_MANIFEST",
                reason,
                "entry collides under the destination comparison profile",
            )

    if kind == FILE_KIND and node.children:
        fail(
            "INVALID_MANIFEST",
            "FILE_ANCESTOR_CONFLICT",
            "new file would be an ancestor of an existing entry",
        )
    node.kind = kind
    node.wire_path = wire_path


def fixed_width_path(index: int, index_width: int, path_bytes: int) -> str:
    token = f"p{index:0{index_width}d}"
    if len(token) > path_bytes or len(str(index)) > index_width:
        fail(
            "FIXTURE_INVALID",
            "GENERATED_PATH",
            "generated index does not fit path recipe",
        )
    component_count = (path_bytes + 256) // 256
    character_count = path_bytes - (component_count - 1)
    component_lengths: list[int] = []
    remaining = character_count
    for slot in range(component_count):
        slots_left = component_count - slot
        length = min(MAX_COMPONENT_BYTES, remaining - (slots_left - 1))
        component_lengths.append(length)
        remaining -= length
    if component_lengths[0] < len(token) or remaining != 0:
        fail("FIXTURE_INVALID", "GENERATED_PATH", "invalid fixed-width path recipe")

    components = [token + "a" * (component_lengths[0] - len(token))]
    components.extend(
        chr(ord("b") + offset % 24) * length
        for offset, length in enumerate(component_lengths[1:])
    )
    path = "/".join(components)
    if len(path.encode("utf-8")) != path_bytes:
        fail("FIXTURE_INVALID", "GENERATED_PATH", "path recipe length drifted")
    return path


def generated_entries(
    recipe_value: Any,
    offer_transfer_id_hex: str,
    defaults: dict[str, Any],
) -> Iterator[dict[str, Any]]:
    if recipe_value is None:
        return
    recipe = require_object(recipe_value, "generated_entries")
    if recipe.get("type") != "fixed_path_series":
        fail("FIXTURE_INVALID", "ENTRY_GENERATOR", "unknown entry generator")
    count = require_integer(
        recipe.get("count"), "generated_entries.count", U32_MAX, error="FIXTURE_INVALID"
    )
    start_index = require_integer(
        recipe.get("start_index", 0),
        "generated_entries.start_index",
        U32_MAX,
        error="FIXTURE_INVALID",
    )
    path_bytes = require_integer(
        recipe.get("path_bytes"),
        "generated_entries.path_bytes",
        U32_MAX,
        error="FIXTURE_INVALID",
    )
    index_width = require_integer(
        recipe.get("index_width"),
        "generated_entries.index_width",
        20,
        error="FIXTURE_INVALID",
    )
    kind = require_integer(
        recipe.get("kind"), "generated_entries.kind", 255, error="FIXTURE_INVALID"
    )
    file_size = require_integer(
        recipe.get("file_size", 0),
        "generated_entries.file_size",
        U64_MAX,
        error="FIXTURE_INVALID",
    )
    transfer_id_hex = recipe.get("transfer_id_hex", offer_transfer_id_hex)
    commitment_hex = recipe.get(
        "file_commitment_hex", defaults.get("file_commitment_hex")
    )

    for offset in range(count):
        entry = {
            "transfer_id_hex": transfer_id_hex,
            "entry_index": start_index + offset,
            "kind": kind,
            "relative_path": fixed_width_path(
                start_index + offset, index_width, path_bytes
            ),
            "file_size": file_size,
        }
        if kind == FILE_KIND and commitment_hex is not None:
            entry["file_commitment_hex"] = commitment_hex
        yield entry


def iter_entries(
    manifest: dict[str, Any],
    offer_transfer_id_hex: str,
    defaults: dict[str, Any],
) -> Iterator[dict[str, Any]]:
    yield from generated_entries(
        manifest.get("generated_entries"), offer_transfer_id_hex, defaults
    )
    explicit = manifest.get("entries", [])
    if not isinstance(explicit, list):
        fail("FIXTURE_INVALID", "ENTRIES_REQUIRED", "entries is not a list")
    for value in explicit:
        entry = require_object(value, "entry")
        if "transfer_id_hex" not in entry:
            entry = dict(entry)
            entry["transfer_id_hex"] = offer_transfer_id_hex
        yield entry


class ManifestValidator:
    def __init__(
        self,
        profile: ComparisonProfile,
        defaults: dict[str, Any],
    ) -> None:
        self.profile = profile
        self.defaults = defaults
        self.path_root = PathNode()
        self.observed_count = 0
        self.aggregate_path_bytes = 0
        self.total_file_bytes = 0

    def validate_offer(
        self, offer: dict[str, Any]
    ) -> tuple[bytes, int, int, bytes, str]:
        transfer_id = resolve_hex(
            offer, "transfer_id_hex", self.defaults, "transfer_id_hex"
        )
        if len(transfer_id) != 16:
            fail(
                "INVALID_OFFER",
                "TRANSFER_ID_LENGTH",
                "offer transfer ID is not 16 bytes",
            )
        entry_count = require_integer(
            offer.get("entry_count"),
            "offer.entry_count",
            U32_MAX,
            error="INVALID_OFFER",
            reason="OFFER_ENTRY_COUNT_RANGE",
        )
        if not 1 <= entry_count <= MAX_ENTRIES:
            fail(
                "INVALID_OFFER",
                "OFFER_ENTRY_COUNT_LIMIT",
                "offer entry count is outside v1 limits",
            )
        total_file_bytes = require_integer(
            offer.get("total_file_bytes"),
            "offer.total_file_bytes",
            U64_MAX,
            error="INVALID_OFFER",
            reason="OFFER_TOTAL_RANGE",
        )
        if total_file_bytes > MAX_TOTAL_FILE_BYTES:
            fail(
                "INVALID_OFFER",
                "OFFER_TOTAL_FILE_BYTES_LIMIT",
                "offer total exceeds 16 TiB",
            )
        commitment = resolve_hex(
            offer,
            "manifest_commitment_hex",
            self.defaults,
            "manifest_commitment_hex",
        )
        validate_commitment(
            commitment,
            error="INVALID_OFFER",
            reason="OFFER_COMMITMENT_LENGTH",
            name="offer commitment",
        )
        return (
            transfer_id,
            entry_count,
            total_file_bytes,
            commitment,
            transfer_id.hex(),
        )

    def validate_entry(self, entry: dict[str, Any], transfer_id: bytes) -> None:
        self.observed_count = checked_add_u64(
            self.observed_count,
            1,
            overflow_reason="MANIFEST_ENTRY_COUNT_OVERFLOW",
        )
        if self.observed_count > MAX_ENTRIES:
            fail(
                "LIMIT_EXCEEDED",
                "MANIFEST_ENTRY_COUNT_LIMIT",
                "observed manifest entries exceed 100000",
            )

        entry_transfer_id = decode_hex(
            entry.get("transfer_id_hex"), "entry.transfer_id_hex"
        )
        if len(entry_transfer_id) != 16 or entry_transfer_id != transfer_id:
            fail(
                "INVALID_MANIFEST",
                "ENTRY_TRANSFER_ID_MISMATCH",
                "entry transfer ID differs from offer",
            )
        entry_index = require_integer(
            entry.get("entry_index"), "entry.entry_index", U32_MAX
        )
        expected_index = self.observed_count - 1
        if entry_index != expected_index:
            fail(
                "INVALID_MANIFEST",
                "ENTRY_INDEX_NONCONTIGUOUS",
                f"expected entry index {expected_index}, got {entry_index}",
            )
        kind = require_integer(entry.get("kind"), "entry.kind", 255)
        if kind not in {FILE_KIND, DIRECTORY_KIND}:
            fail(
                "INVALID_MANIFEST",
                "ENTRY_KIND_INVALID",
                "only file and directory kinds are representable",
            )
        file_size = require_integer(
            entry.get("file_size"), "entry.file_size", U64_MAX
        )
        if file_size > MAX_FILE_BYTES:
            fail(
                "LIMIT_EXCEEDED",
                "FILE_SIZE_LIMIT",
                "entry file size exceeds 16 TiB",
            )

        commitment_value = entry.get("file_commitment_hex")
        if kind == FILE_KIND:
            if commitment_value is None:
                fail(
                    "INVALID_MANIFEST",
                    "FILE_COMMITMENT_REQUIRED",
                    "file entry has no commitment",
                )
            commitment = decode_hex(commitment_value, "entry.file_commitment_hex")
            validate_commitment(
                commitment,
                error="INVALID_MANIFEST",
                reason="FILE_COMMITMENT_LENGTH",
                name="file commitment",
            )
        else:
            if file_size != 0:
                fail(
                    "INVALID_MANIFEST",
                    "DIRECTORY_SIZE_NONZERO",
                    "directory entry size is not zero",
                )
            if commitment_value is not None:
                fail(
                    "INVALID_MANIFEST",
                    "DIRECTORY_COMMITMENT_PRESENT",
                    "directory entry carries a file commitment",
                )

        path_bytes, wire_path, components = validate_wire_path(entry)
        self.aggregate_path_bytes = checked_add_u64(
            self.aggregate_path_bytes,
            path_bytes,
            overflow_reason="AGGREGATE_PATH_BYTES_OVERFLOW",
        )
        if self.aggregate_path_bytes > MAX_AGGREGATE_PATH_BYTES:
            fail(
                "LIMIT_EXCEEDED",
                "AGGREGATE_PATH_BYTES_LIMIT",
                "aggregate encoded path bytes exceed the v1 limit",
            )
        self.total_file_bytes = checked_add_u64(
            self.total_file_bytes,
            file_size,
            overflow_reason="TOTAL_FILE_BYTES_OVERFLOW",
        )
        if self.total_file_bytes > MAX_TOTAL_FILE_BYTES:
            fail(
                "LIMIT_EXCEEDED",
                "TOTAL_FILE_BYTES_LIMIT",
                "incremental file byte total exceeds 16 TiB",
            )
        insert_path(
            self.path_root,
            wire_path,
            canonical_path(components, self.profile),
            kind,
        )

    def validate_end(
        self,
        end: dict[str, Any],
        transfer_id: bytes,
        offer_count: int,
        offer_total: int,
        offer_commitment: bytes,
    ) -> None:
        end_transfer_id = resolve_hex(
            end, "transfer_id_hex", self.defaults, "transfer_id_hex"
        )
        if len(end_transfer_id) != 16 or end_transfer_id != transfer_id:
            fail(
                "INVALID_MANIFEST",
                "END_TRANSFER_ID_MISMATCH",
                "manifest end transfer ID differs from offer",
            )
        end_count = require_integer(
            end.get("entry_count"), "end.entry_count", U32_MAX
        )
        end_total = require_integer(
            end.get("total_file_bytes"), "end.total_file_bytes", U64_MAX
        )
        end_commitment = resolve_hex(
            end,
            "manifest_commitment_hex",
            self.defaults,
            "manifest_commitment_hex",
        )
        validate_commitment(
            end_commitment,
            error="INVALID_MANIFEST",
            reason="END_COMMITMENT_LENGTH",
            name="manifest end commitment",
        )

        if offer_count != self.observed_count:
            fail(
                "INVALID_MANIFEST",
                "OFFER_ENTRY_COUNT_MISMATCH",
                "offer count differs from observed entries",
            )
        if end_count != self.observed_count:
            fail(
                "INVALID_MANIFEST",
                "END_ENTRY_COUNT_MISMATCH",
                "manifest end count differs from observed entries",
            )
        if offer_total != self.total_file_bytes:
            fail(
                "INVALID_MANIFEST",
                "OFFER_TOTAL_FILE_BYTES_MISMATCH",
                "offer total differs from checked file byte sum",
            )
        if end_total != self.total_file_bytes:
            fail(
                "INVALID_MANIFEST",
                "END_TOTAL_FILE_BYTES_MISMATCH",
                "manifest end total differs from checked file byte sum",
            )
        if end_commitment != offer_commitment:
            fail(
                "INVALID_MANIFEST",
                "MANIFEST_COMMITMENT_MISMATCH",
                "manifest end commitment differs from offer",
            )

    def validate(self, manifest_value: Any) -> None:
        manifest = require_object(manifest_value, "manifest")
        offer = require_object(manifest.get("offer"), "offer")
        (
            transfer_id,
            offer_count,
            offer_total,
            offer_commitment,
            transfer_id_hex,
        ) = self.validate_offer(offer)

        for entry in iter_entries(manifest, transfer_id_hex, self.defaults):
            self.validate_entry(entry, transfer_id)

        if "end" not in manifest:
            fail(
                "INVALID_MANIFEST",
                "MANIFEST_END_MISSING",
                "manifest has no terminal summary",
            )
        end = require_object(manifest["end"], "end")
        self.validate_end(
            end,
            transfer_id,
            offer_count,
            offer_total,
            offer_commitment,
        )


def load_profiles(value: Any) -> dict[str, ComparisonProfile]:
    profiles_value = require_object(value, "comparison_profiles")
    profiles: dict[str, ComparisonProfile] = {}
    for name, profile_value in profiles_value.items():
        if not isinstance(name, str) or not name:
            fail("FIXTURE_INVALID", "COMPARISON_PROFILE", "profile name is invalid")
        profile_object = require_object(profile_value, f"profile {name}")
        normalization = profile_object.get("normalization")
        case = profile_object.get("case")
        if normalization != "NFC" or case not in {"sensitive", "ascii_insensitive"}:
            fail(
                "FIXTURE_INVALID",
                "COMPARISON_PROFILE",
                f"profile {name} has unsupported rules",
            )
        profiles[name] = ComparisonProfile(normalization, case)
    return profiles


def validate_case(
    case: dict[str, Any],
    profiles: dict[str, ComparisonProfile],
    defaults: dict[str, Any],
) -> tuple[bool, str]:
    expected = require_object(case.get("expect"), "expect")
    expected_result = expected.get("result")
    expected_reason = expected.get("reason")
    profile_name = case.get("comparison_profile")
    if profile_name not in profiles:
        return False, f"unknown comparison profile {profile_name!r}"

    actual_error: VectorError | None = None
    try:
        ManifestValidator(profiles[profile_name], defaults).validate(
            case.get("manifest")
        )
    except VectorError as error:
        actual_error = error

    if expected_result == "accept":
        if expected_reason != "VALID":
            return False, "accepted vector must use reason VALID"
        if actual_error is not None:
            return False, f"unexpected {actual_error}"
        return True, "VALID"

    if expected_result != "reject":
        return False, f"invalid expected result {expected_result!r}"
    expected_error = expected.get("error")
    if not isinstance(expected_error, str) or not isinstance(expected_reason, str):
        return False, "rejected vector must name error and reason"
    if actual_error is None:
        return (
            False,
            f"expected {expected_error}/{expected_reason}, but manifest was accepted",
        )
    if (actual_error.error, actual_error.reason) != (
        expected_error,
        expected_reason,
    ):
        return (
            False,
            f"expected {expected_error}/{expected_reason}, got {actual_error}",
        )
    return True, f"{actual_error.error}/{actual_error.reason}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "vectors",
        nargs="?",
        type=Path,
        default=Path(__file__).with_name("vectors.json"),
    )
    args = parser.parse_args()

    try:
        with args.vectors.open("r", encoding="utf-8") as source:
            vectors = json.load(source)
        if vectors.get("format_version") != 1:
            print("unsupported manifest fixture format", file=sys.stderr)
            return 2
        defaults = require_object(vectors.get("defaults"), "defaults")
        profiles = load_profiles(vectors.get("comparison_profiles"))
        cases = vectors.get("cases")
        if not isinstance(cases, list):
            fail("FIXTURE_INVALID", "CASES_REQUIRED", "cases is not a list")
    except (OSError, json.JSONDecodeError, VectorError) as error:
        print(f"cannot load manifest vectors: {error}", file=sys.stderr)
        return 2

    failures = 0
    names: set[str] = set()
    for case_value in cases:
        try:
            case = require_object(case_value, "case")
        except VectorError as error:
            print(f"[FAIL] invalid case: {error}")
            failures += 1
            continue
        name = case.get("name")
        if not isinstance(name, str) or not name or name in names:
            print(f"[FAIL] invalid or duplicate case name: {name!r}")
            failures += 1
            continue
        names.add(name)
        passed, result = validate_case(case, profiles, defaults)
        print(f"[{'PASS' if passed else 'FAIL'}] {name}: {result}")
        failures += not passed

    if failures:
        print(f"{failures} of {len(cases)} manifest vector cases failed.", file=sys.stderr)
        return 1
    print(f"Validated {len(cases)} XnnTransfer v1 manifest vector cases.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
