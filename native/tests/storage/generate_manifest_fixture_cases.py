#!/usr/bin/env python3
"""Generate the XT-027 ownership table from the XT-011 manifest corpus."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any


STORAGE_CASES = {
    "legal_exact_path_and_component_byte_limits": (
        "generated",
        "kNone",
        "kNone",
    ),
    "legal_exact_component_count_limit": (0, "kNone", "kNone"),
    "legal_exact_file_total_and_commitment_limits": (
        0,
        "kNone",
        "kNone",
    ),
    "hostile_empty_path": (0, "kPathEmpty", "kPathEmpty"),
    "hostile_posix_absolute_path": (
        0,
        "kPathAbsolute",
        "kPathAbsolute",
    ),
    "hostile_slash_unc_path": (0, "kPathUnc", "kPathUnc"),
    "hostile_backslash_unc_path": (0, "kPathUnc", "kPathUnc"),
    "hostile_windows_drive_absolute_path": (
        0,
        "kPathDriveAbsolute",
        "kPathDriveAbsolute",
    ),
    "hostile_windows_drive_qualified_path": (
        0,
        "kPathDriveQualified",
        "kPathDriveQualified",
    ),
    "hostile_leading_parent_traversal": (
        0,
        "kPathTraversal",
        "kPathTraversal",
    ),
    "hostile_embedded_parent_traversal": (
        0,
        "kPathTraversal",
        "kPathTraversal",
    ),
    "hostile_dot_component": (
        0,
        "kPathDotComponent",
        "kPathDotComponent",
    ),
    "hostile_empty_component": (
        0,
        "kPathEmptyComponent",
        "kPathEmptyComponent",
    ),
    "hostile_trailing_separator": (
        0,
        "kPathTrailingSeparator",
        "kPathTrailingSeparator",
    ),
    "hostile_backslash_separator": (
        0,
        "kPathBackslash",
        "kPathBackslash",
    ),
    "hostile_windows_alternate_data_stream": (
        0,
        "kPathColonOrAds",
        "kPathColonOrAds",
    ),
    "hostile_invalid_utf8_continuation": (
        0,
        "kInvalidUtf8",
        "kInvalidUtf8",
    ),
    "hostile_overlong_utf8": (
        0,
        "kInvalidUtf8",
        "kInvalidUtf8",
    ),
    "hostile_utf8_surrogate": (
        0,
        "kInvalidUtf8",
        "kInvalidUtf8",
    ),
    "hostile_nul_path": (0, "kPathNul", "kPathNul"),
    "hostile_c0_control": (
        0,
        "kPathC0Control",
        "kPathC0Control",
    ),
    "hostile_c1_control": (
        0,
        "kPathC1Control",
        "kPathC1Control",
    ),
    "hostile_unicode_noncharacter": (
        0,
        "kPathNoncharacter",
        "kPathNoncharacter",
    ),
    "hostile_non_nfc_normalization_collision_attempt": (
        1,
        "kPathNotNfc",
        "kPathNotNfc",
    ),
    "hostile_relative_path_byte_limit_plus_one": (
        "generated",
        "kPathBytesLimit",
        "kPathBytesLimit",
    ),
    "hostile_component_byte_limit_plus_one": (
        0,
        "kPathComponentBytesLimit",
        "kPathComponentBytesLimit",
    ),
    "hostile_component_count_limit_plus_one": (
        0,
        "kPathComponentCountLimit",
        "kPathComponentCountLimit",
    ),
    "hostile_file_size_limit_plus_one": (
        0,
        "kNone",
        "kDeclaredSizeLimit",
    ),
}

XT_028_CASES = {
    "hostile_offer_zero_entries",
    "hostile_offer_total_file_bytes_limit_plus_one",
    "hostile_offer_total_u64_overflow",
    "hostile_offer_commitment_too_short",
    "hostile_file_without_commitment",
    "hostile_file_commitment_too_short",
    "hostile_file_size_u64_overflow",
    "hostile_entry_transfer_id_mismatch",
    "hostile_manifest_commitment_summary_mismatch",
    "hostile_end_transfer_id_mismatch",
    "hostile_missing_manifest_end",
}

XT_033_CASES = {
    "legal_files_directories_empty_and_nested",
    "legal_posix_case_distinct_paths",
    "legal_nfc_compatibility_distinct_paths",
    "legal_exact_manifest_entry_count_limit",
    "legal_exact_aggregate_path_bytes_limit",
    "hostile_exact_duplicate_path",
    "hostile_windows_ascii_case_collision",
    "hostile_file_ancestor_before_descendant",
    "hostile_file_ancestor_after_descendant",
    "hostile_windows_casefolded_file_ancestor",
    "hostile_offer_entry_count_limit_plus_one",
    "hostile_offer_entry_count_u32_overflow",
    "hostile_symlink_kind_representation",
    "hostile_hard_link_kind_representation",
    "hostile_device_kind_representation",
    "hostile_socket_kind_representation",
    "hostile_directory_with_commitment",
    "hostile_directory_with_nonzero_size",
    "hostile_noncontiguous_entry_index",
    "hostile_duplicate_entry_index",
    "hostile_incremental_manifest_entry_count_limit",
    "hostile_incremental_aggregate_path_limit_plus_one",
    "hostile_incremental_total_file_bytes_limit_plus_one",
    "hostile_offer_entry_count_summary_mismatch",
    "hostile_end_entry_count_summary_mismatch",
    "hostile_offer_total_summary_mismatch",
    "hostile_end_total_summary_mismatch",
}


def fixed_width_path(
    index: int, index_width: int, path_bytes: int
) -> str:
    token = f"p{index:0{index_width}d}"
    component_count = (path_bytes + 256) // 256
    character_count = path_bytes - (component_count - 1)
    component_lengths: list[int] = []
    remaining = character_count
    for slot in range(component_count):
        slots_left = component_count - slot
        length = min(255, remaining - (slots_left - 1))
        component_lengths.append(length)
        remaining -= length
    if component_lengths[0] < len(token) or remaining != 0:
        raise ValueError("invalid generated path recipe")
    components = [
        token + "a" * (component_lengths[0] - len(token))
    ]
    components.extend(
        chr(ord("b") + offset % 24) * length
        for offset, length in enumerate(component_lengths[1:])
    )
    path = "/".join(components)
    if len(path.encode("utf-8")) != path_bytes:
        raise ValueError("generated path recipe length drifted")
    return path


def decode_path(entry: dict[str, Any]) -> bytes:
    if "relative_path" in entry:
        return entry["relative_path"].encode("utf-8")
    if "relative_path_hex" in entry:
        return bytes.fromhex(entry["relative_path_hex"])
    repeat = entry["relative_path_repeat"]
    return (repeat["text"] * repeat["count"]).encode("utf-8")


def selected_entry(
    manifest: dict[str, Any], selector: int | str
) -> dict[str, Any]:
    if selector == "generated":
        recipe = manifest["generated_entries"]
        return {
            "relative_path": fixed_width_path(
                recipe.get("start_index", 0),
                recipe["index_width"],
                recipe["path_bytes"],
            ),
            "file_size": recipe.get("file_size", 0),
        }
    return manifest["entries"][selector]


def generate(vectors_path: Path) -> str:
    raw = vectors_path.read_bytes()
    vectors = json.loads(raw)
    cases = vectors["cases"]
    names = {case["name"] for case in cases}
    owners = set(STORAGE_CASES) | XT_028_CASES | XT_033_CASES
    if len(cases) != 66:
        raise ValueError(
            f"expected 66 XT-011 cases, got {len(cases)}"
        )
    if names != owners:
        missing = sorted(names - owners)
        stale = sorted(owners - names)
        raise ValueError(
            "fixture ownership drift: "
            f"unclassified={missing}, stale={stale}"
        )
    if (
        set(STORAGE_CASES) & XT_028_CASES
        or set(STORAGE_CASES) & XT_033_CASES
        or XT_028_CASES & XT_033_CASES
    ):
        raise ValueError("fixture ownership sets overlap")

    rows: list[str] = []
    for case in cases:
        name = case["name"]
        reason = case["expect"]["reason"]
        if name in STORAGE_CASES:
            selector, path_error, request_error = STORAGE_CASES[name]
            entry = selected_entry(case["manifest"], selector)
            encoded = decode_path(entry)
            declared_size = entry.get("file_size", 0)
            owner = "kStorage"
        elif name in XT_028_CASES:
            encoded = b""
            declared_size = 0
            path_error = "kNone"
            request_error = "kNone"
            owner = "kXt028"
        else:
            encoded = b""
            declared_size = 0
            path_error = "kNone"
            request_error = "kNone"
            owner = "kXt033"
        rows.append(
            "    {"
            f"{json.dumps(name)}, FixtureOwner::{owner}, "
            f"{json.dumps(reason)}, {json.dumps(encoded.hex())}, "
            f"{declared_size}ULL, "
            f"ValidationError::{path_error}, "
            f"ValidationError::{request_error}"
            "},"
        )

    digest = hashlib.sha256(raw).hexdigest()
    return "\n".join(
        [
            (
                "inline constexpr std::string_view "
                f'kManifestFixtureSha256 = "{digest}";'
            ),
            (
                "inline constexpr std::array<ManifestFixtureCase, "
                f"{len(rows)}> kManifestFixtureCases{{{{"
            ),
            *rows,
            "}};",
            (
                "inline constexpr std::size_t "
                "kStorageFixtureCaseCount = "
                f"{len(STORAGE_CASES)};"
            ),
            (
                "inline constexpr std::size_t "
                "kXt028FixtureCaseCount = "
                f"{len(XT_028_CASES)};"
            ),
            (
                "inline constexpr std::size_t "
                "kXt033FixtureCaseCount = "
                f"{len(XT_033_CASES)};"
            ),
            "",
        ]
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    generated = generate(args.input)
    if args.check:
        print(
            "Classified 66 XT-011 cases: "
            f"storage={len(STORAGE_CASES)}, "
            f"XT-028={len(XT_028_CASES)}, "
            f"XT-033={len(XT_033_CASES)}."
        )
        return 0
    if args.output is None:
        parser.error("--output is required unless --check is used")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if (
        not args.output.exists()
        or args.output.read_text() != generated
    ):
        args.output.write_text(generated)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
