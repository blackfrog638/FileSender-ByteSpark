#!/usr/bin/env python3
"""Generate C++ constants from the accepted security v1 vector manifest."""

from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path
from typing import Any, Dict, List, Mapping


EXPECTED_POSITIVE_OPERATIONS = {
    "normalized_negotiation",
    "pair_context",
    "pairing_exporter_input",
    "sas_words",
    "confirmation_exporter_input",
    "confirmation_initiator",
    "confirmation_responder",
    "confirmation_reject_initiator",
    "confirmation_reject_responder",
    "device_identifier_initiator",
    "transport_context",
    "transport_exporter_input",
    "transport_finished_initiator",
    "transport_finished_responder",
    "rotation_context",
    "rotation_proof_old",
    "rotation_proof_new",
}

EXPECTED_NEGATIVE_OPERATIONS = {
    "validate_ed25519_public_key",
    "decode_canonical",
    "verify_context",
    "derive_sas",
    "verify_confirmation",
    "require_trust_commit",
    "derive_device_identifier",
    "verify_device_identifier",
    "verify_device_identifier_text",
    "verify_transport_finished",
    "validate_rotation_counter",
    "verify_output",
}


def cpp_string(value: Any) -> str:
    if value is None:
        value = ""
    if not isinstance(value, str):
        raise ValueError(f"expected string, got {value!r}")
    return json.dumps(value)


def emit_string(lines: List[str], name: str, value: Any) -> None:
    lines.append(
        f"inline constexpr std::string_view {name} = {cpp_string(value)};"
    )


def emit_array(
    lines: List[str], cpp_type: str, name: str, entries: List[str]
) -> None:
    lines.append(
        f"inline constexpr std::array<{cpp_type}, {len(entries)}> {name} = {{{{"
    )
    lines.extend(f"    {entry}," for entry in entries)
    lines.append("}};")


def require_mapping(value: Any, name: str) -> Mapping[str, Any]:
    if not isinstance(value, dict):
        raise ValueError(f"{name} must be an object")
    return value


def generate(manifest_path: Path) -> str:
    manifest = require_mapping(
        json.loads(manifest_path.read_text(encoding="utf-8")), "manifest"
    )
    vectors = manifest.get("vectors")
    negative_vectors = manifest.get("negative_vectors")
    if not isinstance(vectors, list) or len(vectors) != 17:
        raise ValueError("accepted manifest must contain 17 positive vectors")
    if not isinstance(negative_vectors, list) or len(negative_vectors) != 37:
        raise ValueError("accepted manifest must contain 37 negative vectors")

    positive_operations = {case["operation"] for case in vectors}
    negative_operations = {case["operation"] for case in negative_vectors}
    if positive_operations != EXPECTED_POSITIVE_OPERATIONS:
        raise ValueError(
            f"unsupported positive operation drift: {positive_operations}"
        )
    if negative_operations != EXPECTED_NEGATIVE_OPERATIONS:
        raise ValueError(
            f"unsupported negative operation drift: {negative_operations}"
        )

    fixtures = require_mapping(manifest.get("fixtures"), "fixtures")
    fixture = require_mapping(fixtures.get("baseline"), "fixtures.baseline")
    positive: Dict[str, Mapping[str, Any]] = {}
    for case in vectors:
        output = require_mapping(case["expect"]["output"], case["id"])
        positive[case["id"]] = output

    sas = positive["sas-five-words"]
    indices = sas["indices"]
    expected_words = sas["words"]
    wordlist_bytes = manifest_path.with_name("wordlist.txt").read_bytes()
    canonical_wordlist = wordlist_bytes.replace(b"\r\n", b"\n")
    if b"\r" in canonical_wordlist:
        raise ValueError("wordlist contains a bare carriage return")
    expected_wordlist_hash = manifest["profile"]["wordlist_sha256"]
    if hashlib.sha256(canonical_wordlist).hexdigest() != expected_wordlist_hash:
        raise ValueError("wordlist digest differs from the accepted manifest")
    words = canonical_wordlist.decode("ascii").splitlines()
    if len(words) != 2048 or [words[index] for index in indices] != expected_words:
        raise ValueError("SAS indices do not select the accepted words")

    identity = require_mapping(fixture.get("identity"), "identity")
    pairing = require_mapping(fixture.get("pairing"), "pairing")
    transport = require_mapping(fixture.get("transport"), "transport")
    exporter = require_mapping(
        fixture.get("exporter_material"), "exporter_material"
    )
    rotation = require_mapping(fixture.get("rotation"), "rotation")

    lines = [
        "// Generated from protocol/testdata/security/v1/vectors.json.",
        "#pragma once",
        "",
        "namespace golden {",
    ]
    emit_string(lines, "kInitiatorKey", identity["initiator_key_hex"])
    emit_string(lines, "kResponderKey", identity["responder_key_hex"])
    emit_string(lines, "kRotationOldKey", rotation["old_key_hex"])
    emit_string(lines, "kRotationNewKey", rotation["new_key_hex"])
    emit_string(lines, "kInitiatorPairingNonce", pairing["initiator_nonce_hex"])
    emit_string(lines, "kResponderPairingNonce", pairing["responder_nonce_hex"])
    emit_string(
        lines,
        "kInitiatorTransportNonce",
        transport["initiator_session_nonce_hex"],
    )
    emit_string(
        lines,
        "kResponderTransportNonce",
        transport["responder_session_nonce_hex"],
    )
    emit_string(lines, "kTransferSessionId", transport["session_id_hex"])
    emit_string(
        lines,
        "kRawNegotiationTranscript",
        transport["raw_negotiation_transcript_hex"],
    )
    emit_string(lines, "kPairingExporter", exporter["pairing_hex"])
    emit_string(lines, "kConfirmationExporter", exporter["confirmation_hex"])
    emit_string(lines, "kTransportExporter", exporter["transport_hex"])
    lines.append(
        f"inline constexpr std::uint64_t kRotationCounter = "
        f"{rotation['counter']}U;"
    )
    emit_string(lines, "kRotationNonce", rotation["nonce_hex"])

    normalized = positive["normalized-negotiation"]
    emit_string(lines, "kNormalizedNegotiation", normalized["encoded_hex"])
    emit_string(lines, "kNormalizedNegotiationSha256", normalized["sha256_hex"])
    pair_context = positive["pair-context"]
    emit_string(lines, "kPairContext", pair_context["encoded_hex"])
    emit_string(lines, "kPairContextSha256", pair_context["sha256_hex"])
    pairing_exporter = positive["pairing-exporter-input"]
    emit_string(lines, "kPairingExporterLabel", pairing_exporter["label_ascii"])
    emit_string(
        lines, "kPairingExporterContext", pairing_exporter["context_hex"]
    )
    lines.append(
        f"inline constexpr std::size_t kPairingExporterLength = "
        f"{pairing_exporter['length']}U;"
    )
    emit_string(lines, "kSasInformation", sas["hkdf_info_hex"])
    emit_string(lines, "kSasExpanded", sas["hkdf_output_hex"])
    lines.append(
        "inline constexpr std::array<std::uint16_t, 5> kSasIndices = "
        "{{" + ", ".join(f"{value}U" for value in indices) + "}};"
    )
    lines.append(
        "inline constexpr std::array<std::string_view, 5> kSasWords = "
        "{{" + ", ".join(cpp_string(word) for word in expected_words) + "}};"
    )
    confirmation_exporter = positive["confirmation-exporter-input"]
    emit_string(
        lines,
        "kConfirmationExporterLabel",
        confirmation_exporter["label_ascii"],
    )
    emit_string(
        lines,
        "kConfirmationExporterContext",
        confirmation_exporter["context_hex"],
    )
    lines.append(
        f"inline constexpr std::size_t kConfirmationExporterLength = "
        f"{confirmation_exporter['length']}U;"
    )

    for identifier, prefix in (
        ("peer-confirmation-initiator", "kConfirmationInitiator"),
        ("peer-confirmation-responder", "kConfirmationResponder"),
        ("peer-rejection-initiator", "kRejectionInitiator"),
        ("peer-rejection-responder", "kRejectionResponder"),
    ):
        output = positive[identifier]
        emit_string(lines, f"{prefix}Message", output["message_hex"])
        emit_string(lines, f"{prefix}Hmac", output["hmac_sha256_hex"])

    device = positive["device-identifier-initiator"]
    emit_string(lines, "kDeviceIdentifierLabel", device["label_ascii"])
    emit_string(lines, "kDeviceIdentifierPublicKey", device["public_key_hex"])
    emit_string(lines, "kDeviceIdentifierInput", device["canonical_input_hex"])
    emit_string(lines, "kDeviceIdentifierSha256", device["sha256_hex"])

    transport_context = positive["transport-context"]
    emit_string(lines, "kTransportContext", transport_context["encoded_hex"])
    emit_string(
        lines, "kTransportContextSha256", transport_context["sha256_hex"]
    )
    transport_exporter = positive["transport-exporter-input"]
    emit_string(
        lines, "kTransportExporterLabel", transport_exporter["label_ascii"]
    )
    emit_string(
        lines, "kTransportExporterContext", transport_exporter["context_hex"]
    )
    lines.append(
        f"inline constexpr std::size_t kTransportExporterLength = "
        f"{transport_exporter['length']}U;"
    )
    for identifier, prefix in (
        ("transport-finished-initiator", "kFinishedInitiator"),
        ("transport-finished-responder", "kFinishedResponder"),
    ):
        output = positive[identifier]
        emit_string(lines, f"{prefix}Message", output["message_hex"])
        emit_string(lines, f"{prefix}Hmac", output["hmac_sha256_hex"])

    rotation_context = positive["rotation-context"]
    emit_string(lines, "kRotationContext", rotation_context["encoded_hex"])
    emit_string(lines, "kRotationContextSha256", rotation_context["sha256_hex"])
    for identifier, prefix in (
        ("rotation-proof-old-key", "kRotationProofOld"),
        ("rotation-proof-new-key", "kRotationProofNew"),
    ):
        output = positive[identifier]
        emit_string(lines, f"{prefix}Message", output["message_hex"])
        emit_string(lines, f"{prefix}Sha256", output["sha256_hex"])

    grouped: Dict[str, List[Mapping[str, Any]]] = {}
    for case in negative_vectors:
        grouped.setdefault(case["operation"], []).append(case)

    def common(case: Mapping[str, Any]) -> tuple[str, Mapping[str, Any], str]:
        return (
            cpp_string(case["id"]),
            require_mapping(case["input"], case["id"]),
            cpp_string(case["expect"]["error"]),
        )

    entries = []
    for case in grouped["decode_canonical"]:
        identifier, inputs, error = common(case)
        entries.append(
            "{" + ", ".join(
                (identifier, cpp_string(inputs["encoded_hex"]), error)
            ) + "}"
        )
    emit_array(lines, "DecodeCase", "kDecodeCases", entries)

    entries = []
    for case in grouped["validate_ed25519_public_key"]:
        identifier, inputs, error = common(case)
        entries.append(
            "{" + ", ".join(
                (identifier, cpp_string(inputs["public_key_hex"]), error)
            ) + "}"
        )
    emit_array(lines, "KeyCase", "kKeyCases", entries)

    entries = []
    for case in grouped["verify_context"]:
        identifier, inputs, error = common(case)
        entries.append(
            "{" + ", ".join(
                (
                    identifier,
                    cpp_string(inputs["encoded_hex"]),
                    str(inputs["expected_kind"]),
                    cpp_string(inputs["expected_sha256_hex"]),
                    error,
                )
            ) + "}"
        )
    emit_array(lines, "ContextCase", "kContextCases", entries)

    entries = []
    for case in grouped["derive_sas"]:
        identifier, inputs, error = common(case)
        entries.append(
            "{" + ", ".join(
                (
                    identifier,
                    cpp_string(inputs["exporter_hex"]),
                    cpp_string(inputs["pair_context_hex"]),
                    error,
                )
            ) + "}"
        )
    emit_array(lines, "SasCase", "kSasCases", entries)

    entries = []
    for case in grouped["verify_confirmation"]:
        identifier, inputs, error = common(case)
        entries.append(
            "{" + ", ".join(
                (
                    identifier,
                    cpp_string(inputs["key_hex"]),
                    cpp_string(inputs["message_hex"]),
                    cpp_string(inputs["presented_hex"]),
                    cpp_string(inputs["expected_pair_context_hex"]),
                    str(inputs["expected_role"]),
                    error,
                )
            ) + "}"
        )
    emit_array(lines, "ConfirmationCase", "kConfirmationCases", entries)

    entries = []
    for case in grouped["require_trust_commit"]:
        identifier, inputs, error = common(case)
        entries.append(
            "{" + ", ".join(
                (
                    identifier,
                    cpp_string(inputs["key_hex"]),
                    cpp_string(inputs["message_hex"]),
                    cpp_string(inputs["presented_hex"]),
                    cpp_string(inputs["expected_pair_context_hex"]),
                    str(inputs["expected_role"]),
                    str(inputs["local_decision"]),
                    error,
                )
            ) + "}"
        )
    emit_array(lines, "TrustCase", "kTrustCases", entries)

    entries = []
    for case in grouped["derive_device_identifier"]:
        identifier, inputs, error = common(case)
        entries.append(
            "{" + ", ".join(
                (identifier, cpp_string(inputs["canonical_input_hex"]), error)
            ) + "}"
        )
    emit_array(lines, "DeviceInputCase", "kDeviceInputCases", entries)

    entries = []
    for case in grouped["verify_device_identifier"]:
        identifier, inputs, error = common(case)
        entries.append(
            "{" + ", ".join(
                (
                    identifier,
                    cpp_string(inputs["public_key_hex"]),
                    cpp_string(inputs.get("public_key_sha256_hex")),
                    cpp_string(inputs["identifier_public_key_hex"]),
                    cpp_string(inputs["expected_sha256_hex"]),
                    error,
                )
            ) + "}"
        )
    emit_array(lines, "DeviceVerifyCase", "kDeviceVerifyCases", entries)

    entries = []
    for case in grouped["verify_device_identifier_text"]:
        identifier, inputs, error = common(case)
        entries.append(
            "{" + ", ".join(
                (
                    identifier,
                    cpp_string(inputs["public_key_hex"]),
                    cpp_string(inputs["presented_text"]),
                    error,
                )
            ) + "}"
        )
    emit_array(lines, "DeviceTextCase", "kDeviceTextCases", entries)

    entries = []
    for case in grouped["verify_transport_finished"]:
        identifier, inputs, error = common(case)
        entries.append(
            "{" + ", ".join(
                (
                    identifier,
                    cpp_string(inputs["key_hex"]),
                    cpp_string(inputs["message_hex"]),
                    cpp_string(inputs["presented_hex"]),
                    str(inputs["expected_role"]),
                    error,
                )
            ) + "}"
        )
    emit_array(lines, "FinishedCase", "kFinishedCases", entries)

    entries = []
    for case in grouped["validate_rotation_counter"]:
        identifier, inputs, error = common(case)
        entries.append(
            "{" + ", ".join(
                (
                    identifier,
                    f"{inputs['previous']}U",
                    f"{inputs['current']}U",
                    error,
                )
            ) + "}"
        )
    emit_array(lines, "RotationCounterCase", "kRotationCounterCases", entries)

    entries = []
    for case in grouped["verify_output"]:
        identifier, inputs, error = common(case)
        entries.append(
            "{" + ", ".join(
                (
                    identifier,
                    cpp_string(inputs["value_hex"]),
                    cpp_string(inputs["expected_sha256_hex"]),
                    error,
                )
            ) + "}"
        )
    emit_array(lines, "OutputCase", "kOutputCases", entries)

    lines.extend(
        [
            "",
            "}  // namespace golden",
            "",
        ]
    )
    return "\n".join(lines)


def main() -> int:
    if len(sys.argv) != 3:
        print(
            "usage: generate_golden_vectors.py VECTORS_JSON OUTPUT_HEADER",
            file=sys.stderr,
        )
        return 2
    manifest_path = Path(sys.argv[1])
    output_path = Path(sys.argv[2])
    try:
        generated = generate(manifest_path)
        output_path.parent.mkdir(parents=True, exist_ok=True)
        with output_path.open("w", encoding="utf-8", newline="\n") as output:
            output.write(generated)
    except (OSError, KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
        print(f"security vector generation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
