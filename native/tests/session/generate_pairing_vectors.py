#!/usr/bin/env python3

import json
import pathlib
import sys


def symbol(name: str) -> str:
    return "k" + "".join(part.capitalize() for part in name.split("_"))


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit(
            "usage: generate_pairing_vectors.py PAIRING_INPUT SECURITY_INPUT OUTPUT"
        )

    source = pathlib.Path(sys.argv[1])
    security_source = pathlib.Path(sys.argv[2])
    destination = pathlib.Path(sys.argv[3])
    document = json.loads(source.read_text(encoding="utf-8"))
    security = json.loads(security_source.read_text(encoding="utf-8"))

    lines = [
        "#ifndef XNN_TRANSFER_TESTS_SESSION_PAIRING_VECTORS_HPP_",
        "#define XNN_TRANSFER_TESTS_SESSION_PAIRING_VECTORS_HPP_",
        "",
        "#include <string_view>",
        "",
        "namespace pairing_vectors {",
        "",
    ]
    for name, frame in sorted(document["frames"].items()):
        lines.append(
            f'inline constexpr std::string_view {symbol(name)} = "{frame["hex"]}";'
        )

    baseline = document["fixtures"]["baseline"]
    normalized = next(
        vector
        for vector in security["vectors"]
        if vector["id"] == "normalized-negotiation"
    )
    transport = security["fixtures"]["baseline"]["transport"]
    lines.extend(
        [
            "",
            "inline constexpr std::string_view kInitiatorKey = "
            f'"{baseline["initiator_identity_key_hex"]}";',
            "inline constexpr std::string_view kResponderKey = "
            f'"{baseline["responder_identity_key_hex"]}";',
            "inline constexpr std::string_view kConfirmationExporter = "
            f'"{baseline["confirmation_exporter_hex"]}";',
            'inline constexpr std::string_view kPairContext = '
            f'"{document["cases"][0]["expected_pair_context_hex"]}";',
            "inline constexpr std::string_view kNormalizedNegotiation = "
            f'"{normalized["expect"]["output"]["encoded_hex"]}";',
            "inline constexpr std::string_view kInitiatorTransportNonce = "
            f'"{transport["initiator_session_nonce_hex"]}";',
            "inline constexpr std::string_view kResponderTransportNonce = "
            f'"{transport["responder_session_nonce_hex"]}";',
            "inline constexpr std::string_view kTransportSessionId = "
            f'"{transport["session_id_hex"]}";',
            "inline constexpr std::string_view kRawNegotiationTranscript = "
            f'"{transport["raw_negotiation_transcript_hex"]}";',
            "",
            "}  // namespace pairing_vectors",
            "",
            "#endif  // XNN_TRANSFER_TESTS_SESSION_PAIRING_VECTORS_HPP_",
            "",
        ]
    )
    destination.write_text("\n".join(lines), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
