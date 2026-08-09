#!/usr/bin/env python3
"""Validate the deterministic XnnTransfer v1 security-profile vectors."""

from __future__ import annotations

import argparse
import hashlib
import hmac
import json
import re
import struct
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Iterable, List, Mapping, Optional, Sequence, Tuple


MAGIC = b"XNNS"
CANONICAL_VERSION = 1
MAX_CANONICAL_LENGTH = 1_048_576
MAX_FIELDS = 32
WORD_COUNT = 2_048

ED25519_FIELD_PRIME = 2**255 - 19
ED25519_D = (
    -121665 * pow(121666, ED25519_FIELD_PRIME - 2, ED25519_FIELD_PRIME)
) % ED25519_FIELD_PRIME
ED25519_SQRT_M1 = pow(
    2, (ED25519_FIELD_PRIME - 1) // 4, ED25519_FIELD_PRIME
)
ED25519_SUBGROUP_ORDER = (
    2**252 + 27742317777372353535851937790883648493
)
EdwardsPoint = Tuple[int, int, int, int]
ED25519_IDENTITY: EdwardsPoint = (0, 1, 1, 0)

SECURITY_PROFILE_ID = 1
ROLE_INITIATOR = 1
ROLE_RESPONDER = 2
DECISION_REJECT = 0
DECISION_CONFIRM = 1
SIGNER_OLD_KEY = 1
SIGNER_NEW_KEY = 2

KIND_NEGOTIATION = 1
KIND_PAIR_CONTEXT = 2
KIND_SAS_INFO = 3
KIND_TRANSPORT_CONTEXT = 4
KIND_ROTATION_CONTEXT = 5
KIND_ROTATION_PROOF = 6
KIND_DEVICE_IDENTIFIER = 7

BASE_TRANSFER_V1 = 0x00010001

PAIRING_MAGIC = b"XNNP"
PAIRING_FIXED_HEADER_LENGTH = 20
PAIRING_ALPN = b"xnn-transfer-pairing/1"
TRANSPORT_ALPN = b"xnn-transfer/1"
PAIRING_MAX_BODY_LENGTH = 4_096
PAIRING_MAX_FRAME_LENGTH = 4_116
PAIRING_MAX_FIELDS = 16
PAIRING_MAX_MESSAGES = 16
PAIRING_MAX_BYTES = 65_536

PAIRING_HELLO = 0x0001
PAIRING_SELECT = 0x0002
PAIRING_SELECT_ACK = 0x0003
PAIRING_DECISION = 0x0004
PAIRING_ABORT = 0x0005

PAIRING_WIRE_U8 = 1
PAIRING_WIRE_U16 = 2
PAIRING_WIRE_BYTES = 5

PAIRING_INITIATOR_TO_RESPONDER = "initiator_to_responder"
PAIRING_RESPONDER_TO_INITIATOR = "responder_to_initiator"
PAIRING_DIRECTIONS = {
    PAIRING_INITIATOR_TO_RESPONDER,
    PAIRING_RESPONDER_TO_INITIATOR,
}

PAIRING_EXPECTED_CONTRACT = {
    "pairing_alpn_hex": PAIRING_ALPN.hex(),
    "pairing_alpn_length": len(PAIRING_ALPN),
    "transport_alpn_hex": TRANSPORT_ALPN.hex(),
    "transport_alpn_length": len(TRANSPORT_ALPN),
    "security_profile_hex": "0001",
    "magic_hex": PAIRING_MAGIC.hex(),
    "fixed_header_length": PAIRING_FIXED_HEADER_LENGTH,
    "pairing_version": [1, 0],
    "max_body_length": PAIRING_MAX_BODY_LENGTH,
    "max_frame_length": PAIRING_MAX_FRAME_LENGTH,
    "max_fields": PAIRING_MAX_FIELDS,
    "max_inbound_messages_per_endpoint": PAIRING_MAX_MESSAGES,
    "max_inbound_bytes_per_endpoint": PAIRING_MAX_BYTES,
    "max_certificate_der": 4_096,
    "max_certificate_list_contents": 8_192,
    "max_certificate_handshake_message": 8_200,
    "certificate_request_context_length": 0,
    "certificate_chain_length": 1,
    "max_incomplete_handshakes_global": 8,
    "max_incomplete_handshakes_per_source": 2,
    "max_visible_attempts": 1,
    "max_replay_entries_global": 1_024,
    "max_replay_entries_per_remote_key": 32,
    "replay_retention_seconds": 600,
    "pairing_window_seconds": 120,
    "handshake_timeout_seconds": 5,
    "first_frame_timeout_seconds": 5,
    "selection_timeout_seconds": 10,
    "frame_assembly_timeout_seconds": 10,
    "idle_timeout_seconds": 30,
    "decision_timeout_seconds": 90,
    "pairing_timeout_seconds": 120,
    "abort_flush_timeout_seconds": 1,
    "global_bucket_capacity": 16,
    "global_bucket_refill_per_second": 1,
    "source_bucket_capacity": 4,
    "source_bucket_refill_seconds": 15,
    "public_abort_codes": {
        "FAILED": 1,
        "BUSY": 2,
        "CANCELLED": 3,
        "TIMEOUT": 4,
    },
    "local_terminal_codes": {
        "PAIRING_MALFORMED": 0x1001,
        "PAIRING_LIMIT_EXCEEDED": 0x1002,
        "PAIRING_SEQUENCE_VIOLATION": 0x1003,
        "PAIRING_UNSUPPORTED_VERSION": 0x1004,
        "PAIRING_UNSUPPORTED_PROFILE": 0x1005,
        "PAIRING_DOWNGRADE_DETECTED": 0x1006,
        "PAIRING_ROLE_MISMATCH": 0x1007,
        "PAIRING_STATE_VIOLATION": 0x1008,
        "PAIRING_REPLAY_DETECTED": 0x1009,
        "PAIRING_CONFIRMATION_FAILED": 0x100A,
        "PAIRING_AUTHENTICATED_REJECT": 0x100B,
        "PAIRING_LOCAL_REJECT": 0x100C,
        "PAIRING_ALREADY_DECIDED": 0x100D,
        "PAIRING_CANCELLED": 0x100E,
        "PAIRING_TIMEOUT": 0x100F,
        "PAIRING_BUSY": 0x1010,
        "PAIRING_CERTIFICATE_REJECTED": 0x1011,
        "PAIRING_INTERNAL_FAILURE": 0x1012,
    },
}

PAIRING_EXPECTED_RECONNECT = {
    "session_id_hex": "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff",
    "normalized_negotiation_sha256_hex": (
        "00200c2aa0cbc45a821e0896e29e03f6a61c3546d5b1563d04a9598d9a9ed741"
    ),
    "raw_negotiation_transcript_sha256_hex": (
        "d4fa3b7b5fe479187b503f3cf1148f64e16df8a5e78ab0765a6931c4472259d0"
    ),
    "transport_context_hex": (
        "7fcb0313518a55ff34783e3ec2984fb28b1de235f9e7cccb6746c79dee88ed51"
    ),
    "transport_exporter_hex": (
        "e0e1e2e3e4e5e6e7e8e9eaebecedeeeff0f1f2f3f4f5f6f7f8f9fafbfcfdfeff"
    ),
    "initiator_finished_hex": (
        "1cfad1c62e5d6a4ce89de990032c2b65937563aa6e0c8518c90fbadf997ca25c"
    ),
    "responder_finished_hex": (
        "46cc2c4bbf13c8fea7a312995314428148c15fcfc6a37ec5fad3bb58cbeeff87"
    ),
}

PAIRING_REQUIRED_COVERAGE = {
    "first-pairing",
    "authenticated-rejection",
    "paired-reconnect",
    "malformed",
    "duplicate",
    "replay",
    "role-swapped",
    "oversized",
    "unknown-profile",
    "downgrade",
    "missing",
    "reordered",
    "unknown-field",
    "trailing",
    "cancellation",
    "identity-collision",
    "commit-linearization",
    "peer-confirm-first",
    "reconnect-binding",
}

PAIR_CONTEXT_LABEL = b"XnnTransfer pairing v1"
PAIR_EXPORTER_LABEL = b"EXPORTER-XnnTransfer-Pairing-v1"
SAS_INFO_LABEL = b"XnnTransfer SAS words v1"
CONFIRMATION_EXPORTER_LABEL = (
    b"EXPORTER-XnnTransfer-Pairing-Confirmation-v1"
)
TRANSPORT_CONTEXT_LABEL = b"XnnTransfer transport v1"
TRANSPORT_EXPORTER_LABEL = b"EXPORTER-XnnTransfer-Transport-v1"
ROTATION_CONTEXT_LABEL = b"XnnTransfer rotation v1"
ROTATION_PROOF_LABEL = b"XnnTransfer rotation proof v1"
DEVICE_IDENTIFIER_LABEL = b"XnnTransfer device identifier v1"

AUTHENTICATED_REJECT = "authenticated_reject"
AFFIRMATIVE_CONFIRM = "affirmative_confirm"

EXPECTED_PROFILE_METADATA = {
    "id": SECURITY_PROFILE_ID,
    "status": "proposed-test-evidence-only",
    "byte_order": "big-endian",
    "canonical_magic": MAGIC.decode("ascii"),
    "canonical_version": CANONICAL_VERSION,
    "hash": "SHA-256",
    "mac": "HMAC-SHA256",
    "sas_kdf": "HKDF-Expand-SHA256",
    "sas_bits": 55,
    "sas_word_count": 5,
    "sas_wordlist": "BIP39-English-index-order",
    "exporter_material": "fixture-input-only",
    "ed25519_public_key_validation": (
        "RFC-8032-section-5.1.3-canonical-decode-"
        "nonidentity-prime-order-subgroup"
    ),
    "ed25519_subgroup_order_decimal": str(ED25519_SUBGROUP_ORDER),
    "device_identifier_label_hex": DEVICE_IDENTIFIER_LABEL.hex(),
    "device_identifier_input": "XNNS-canonical-kind-07",
    "device_identifier_hash": "SHA-256",
    "device_identifier_text": "lowercase-hex-no-prefix",
}

EXPECTED_SUBGROUP_EVIDENCE = {
    "source": "XT-010-independent-review-BR-04",
    "identity_public_key_hex": "01" + "00" * 31,
    "identity_forgery_signature_hex": (
        "5866666666666666666666666666666666666666666666666666666666666666"
        "01"
        + "00" * 31
    ),
    "identity_forgery_equation": "[8]B = [8]B + [8]k(0,1)",
    "identity_forgery_scope": "every-message-without-a-private-key",
    "backend_observations": {
        "OpenSSL-3.6.3": (
            "identity-order2-order4-public-key-import-accepted;"
            "identity-signature-accepted"
        ),
        "Node-OpenSSL": "identity-signature-accepted-for-three-messages",
        "Apple-CryptoKit": "identity-signature-accepted",
        "libsodium": "small-order-and-non-main-subgroup-points-rejected",
    },
}


class VectorError(Exception):
    """A stable failure expected by a negative vector."""

    def __init__(self, code: str, detail: str) -> None:
        super().__init__(f"{code}: {detail}")
        self.code = code
        self.detail = detail


@dataclass(frozen=True)
class CanonicalObject:
    kind: int
    fields: Dict[int, bytes]
    encoded: bytes


@dataclass(frozen=True)
class ConfirmationResult:
    sender_role: int
    decision: int
    authenticated_result: str
    terminal: bool
    trust_commit_permitted: bool


@dataclass(frozen=True)
class PairingFrame:
    message_type: int
    sequence: int
    fields: Dict[int, Any]
    encoded_length: int


def fail(code: str, detail: str) -> None:
    raise VectorError(code, detail)


def require_mapping(value: Any, name: str) -> Mapping[str, Any]:
    if not isinstance(value, dict):
        fail("INVALID_MANIFEST", f"{name} must be an object")
    return value


def require_sequence(value: Any, name: str) -> Sequence[Any]:
    if not isinstance(value, list):
        fail("INVALID_MANIFEST", f"{name} must be an array")
    return value


def require_string(value: Any, name: str) -> str:
    if not isinstance(value, str) or not value:
        fail("INVALID_MANIFEST", f"{name} must be a nonempty string")
    return value


def require_integer(
    value: Any, name: str, minimum: int = 0, maximum: int = 0xFFFFFFFF
) -> int:
    if (
        not isinstance(value, int)
        or isinstance(value, bool)
        or value < minimum
        or value > maximum
    ):
        fail(
            "INVALID_MANIFEST",
            f"{name} must be an integer in [{minimum}, {maximum}]",
        )
    return value


def decode_hex(value: Any, name: str, length: Optional[int] = None) -> bytes:
    if not isinstance(value, str):
        fail("INVALID_MANIFEST", f"{name} must be hexadecimal text")
    try:
        decoded = bytes.fromhex(value)
    except ValueError:
        fail("INVALID_MANIFEST", f"{name} is not valid hexadecimal text")
    if length is not None and len(decoded) != length:
        fail(
            "INVALID_LENGTH",
            f"{name} must contain {length} octets, got {len(decoded)}",
        )
    return decoded


def edwards_add(
    left: EdwardsPoint,
    right: EdwardsPoint,
) -> EdwardsPoint:
    # Complete extended-coordinate addition keeps the subgroup check inversion-free.
    x1, y1, z1, t1 = left
    x2, y2, z2, t2 = right
    a = (y1 - x1) * (y2 - x2) % ED25519_FIELD_PRIME
    b = (y1 + x1) * (y2 + x2) % ED25519_FIELD_PRIME
    c = 2 * ED25519_D * t1 * t2 % ED25519_FIELD_PRIME
    d = 2 * z1 * z2 % ED25519_FIELD_PRIME
    e = (b - a) % ED25519_FIELD_PRIME
    f = (d - c) % ED25519_FIELD_PRIME
    g = (d + c) % ED25519_FIELD_PRIME
    h = (b + a) % ED25519_FIELD_PRIME
    return (
        e * f % ED25519_FIELD_PRIME,
        g * h % ED25519_FIELD_PRIME,
        f * g % ED25519_FIELD_PRIME,
        e * h % ED25519_FIELD_PRIME,
    )


def edwards_scalar_multiply(
    scalar: int,
    point: EdwardsPoint,
) -> EdwardsPoint:
    result = ED25519_IDENTITY
    addend = point
    while scalar:
        if scalar & 1:
            result = edwards_add(result, addend)
        addend = edwards_add(addend, addend)
        scalar >>= 1
    return result


def edwards_is_identity(point: EdwardsPoint) -> bool:
    x, y, z, _ = point
    return (
        x % ED25519_FIELD_PRIME == 0
        and (y - z) % ED25519_FIELD_PRIME == 0
    )


def decode_ed25519_public_key(
    public_key: bytes, name: str
) -> EdwardsPoint:
    if len(public_key) != 32:
        fail("INVALID_LENGTH", f"{name} must contain 32 octets")

    encoded = int.from_bytes(public_key, "little")
    x_sign = encoded >> 255
    y = encoded & ((1 << 255) - 1)
    if y >= ED25519_FIELD_PRIME:
        fail(
            "NON_CANONICAL_ENCODING",
            f"{name} has a noncanonical Edwards25519 y-coordinate",
        )

    y_squared = y * y % ED25519_FIELD_PRIME
    numerator = (y_squared - 1) % ED25519_FIELD_PRIME
    denominator = (ED25519_D * y_squared + 1) % ED25519_FIELD_PRIME
    if denominator == 0:
        fail("INVALID_PUBLIC_KEY", f"{name} is not an Edwards25519 point")

    # Recover x and select the encoded sign exactly as RFC 8032 section 5.1.3.
    x_squared = (
        numerator
        * pow(denominator, ED25519_FIELD_PRIME - 2, ED25519_FIELD_PRIME)
    ) % ED25519_FIELD_PRIME
    x = pow(
        x_squared,
        (ED25519_FIELD_PRIME + 3) // 8,
        ED25519_FIELD_PRIME,
    )
    if x * x % ED25519_FIELD_PRIME != x_squared:
        x = x * ED25519_SQRT_M1 % ED25519_FIELD_PRIME
    if x * x % ED25519_FIELD_PRIME != x_squared:
        fail("INVALID_PUBLIC_KEY", f"{name} is not an Edwards25519 point")
    if x == 0 and x_sign == 1:
        fail(
            "NON_CANONICAL_ENCODING",
            f"{name} sets the sign bit for x=0",
        )
    if (x & 1) != x_sign:
        x = ED25519_FIELD_PRIME - x

    if (
        -x * x
        + y_squared
        - 1
        - ED25519_D * x * x * y_squared
    ) % ED25519_FIELD_PRIME != 0:
        fail("INVALID_PUBLIC_KEY", f"{name} is not an Edwards25519 point")
    return (x, y, 1, x * y % ED25519_FIELD_PRIME)


def validate_ed25519_public_key(public_key: bytes, name: str) -> None:
    point = decode_ed25519_public_key(public_key, name)
    if edwards_is_identity(point):
        fail("INVALID_PUBLIC_KEY", f"{name} is the Edwards25519 identity")
    if not edwards_is_identity(
        edwards_scalar_multiply(ED25519_SUBGROUP_ORDER, point)
    ):
        fail(
            "INVALID_PUBLIC_KEY",
            f"{name} is not in the Ed25519 prime-order subgroup",
        )


def field_schema(kind: int) -> Dict[int, Optional[int]]:
    schemas = {
        KIND_NEGOTIATION: {
            1: 1,
            2: 4,
            3: None,
            4: None,
            5: 10,
            6: 1,
            7: 4,
            8: None,
            9: None,
            10: 10,
            11: 2,
            12: None,
            13: 10,
        },
        KIND_PAIR_CONTEXT: {
            1: len(PAIR_CONTEXT_LABEL),
            2: 1,
            3: 1,
            4: 32,
            5: 32,
            6: 32,
            7: 32,
            8: 2,
            9: None,
        },
        KIND_SAS_INFO: {
            1: len(SAS_INFO_LABEL),
            2: 32,
        },
        KIND_TRANSPORT_CONTEXT: {
            1: len(TRANSPORT_CONTEXT_LABEL),
            2: 1,
            3: 1,
            4: 32,
            5: 32,
            6: 32,
            7: 32,
            8: 2,
            9: None,
            10: None,
            11: 16,
        },
        KIND_ROTATION_CONTEXT: {
            1: len(ROTATION_CONTEXT_LABEL),
            2: 32,
            3: 32,
            4: 8,
            5: 32,
            6: 32,
        },
        KIND_ROTATION_PROOF: {
            1: len(ROTATION_PROOF_LABEL),
            2: 32,
            3: 1,
        },
        KIND_DEVICE_IDENTIFIER: {
            1: len(DEVICE_IDENTIFIER_LABEL),
            2: 32,
        },
    }
    schema = schemas.get(kind)
    if schema is None:
        fail("UNKNOWN_KIND", f"unknown canonical object kind {kind}")
    return schema


def encode_object(kind: int, fields: Iterable[Tuple[int, bytes]]) -> bytes:
    items = list(fields)
    if len(items) > MAX_FIELDS:
        fail("LIMIT_EXCEEDED", "canonical object has too many fields")
    body = bytearray()
    previous = 0
    for field_id, value in items:
        if field_id <= previous:
            fail(
                "NON_CANONICAL_ENCODING",
                "encoder fields must be strictly increasing",
            )
        if not isinstance(value, bytes):
            fail("INVALID_MANIFEST", f"field {field_id} is not bytes")
        body.extend(struct.pack(">HI", field_id, len(value)))
        body.extend(value)
        previous = field_id
    if len(body) > MAX_CANONICAL_LENGTH:
        fail("LIMIT_EXCEEDED", "canonical object body is too large")
    return (
        struct.pack(
            ">4sBBHI",
            MAGIC,
            CANONICAL_VERSION,
            kind,
            len(items),
            len(body),
        )
        + body
    )


def parse_object(
    encoded: bytes, expected_kind: Optional[int] = None
) -> CanonicalObject:
    if len(encoded) < 12:
        fail("MALFORMED_ENCODING", "canonical header is truncated")
    magic, version, kind, field_count, body_length = struct.unpack_from(
        ">4sBBHI", encoded
    )
    if magic != MAGIC:
        fail("MALFORMED_ENCODING", "canonical magic is invalid")
    if version != CANONICAL_VERSION:
        fail("UNSUPPORTED_VERSION", f"canonical version {version} is unsupported")
    if expected_kind is not None and kind != expected_kind:
        fail(
            "DOMAIN_MISMATCH",
            f"expected object kind {expected_kind}, got {kind}",
        )
    schema = field_schema(kind)
    if field_count > MAX_FIELDS:
        fail("LIMIT_EXCEEDED", "canonical field count exceeds the limit")
    if body_length > MAX_CANONICAL_LENGTH:
        fail("LIMIT_EXCEEDED", "canonical body length exceeds the limit")
    if len(encoded) != 12 + body_length:
        fail(
            "MALFORMED_ENCODING",
            "canonical encoded length differs from the declared length",
        )

    fields: Dict[int, bytes] = {}
    offset = 12
    previous = 0
    for _ in range(field_count):
        if len(encoded) - offset < 6:
            fail("MALFORMED_ENCODING", "canonical field header is truncated")
        field_id, value_length = struct.unpack_from(">HI", encoded, offset)
        offset += 6
        if field_id == previous:
            fail("DUPLICATE_FIELD", f"field {field_id} is duplicated")
        if field_id < previous:
            fail("NON_CANONICAL_ENCODING", "fields are out of order")
        if field_id not in schema:
            fail("UNKNOWN_FIELD", f"field {field_id} is not defined for kind {kind}")
        if value_length > len(encoded) - offset:
            fail("MALFORMED_ENCODING", f"field {field_id} value is truncated")
        value = encoded[offset : offset + value_length]
        offset += value_length
        expected_length = schema[field_id]
        if expected_length is not None and len(value) != expected_length:
            fail(
                "INVALID_LENGTH",
                f"field {field_id} must contain {expected_length} octets",
            )
        fields[field_id] = value
        previous = field_id

    if offset != len(encoded):
        fail("TRAILING_DATA", "canonical object has trailing data")
    missing = sorted(set(schema) - set(fields))
    if missing:
        fail("MISSING_FIELD", f"required field {missing[0]} is absent")
    result = CanonicalObject(kind, fields, encoded)
    validate_object(result)
    return result


def encode_capabilities(values: Sequence[Any], name: str) -> bytes:
    encoded_values = [
        require_integer(value, f"{name}[{index}]")
        for index, value in enumerate(values)
    ]
    if encoded_values != sorted(set(encoded_values)):
        fail(
            "NON_CANONICAL_ENCODING",
            f"{name} must be sorted and unique",
        )
    identifiers = [value >> 16 for value in encoded_values]
    if len(set(identifiers)) != len(identifiers):
        fail(
            "NON_CANONICAL_ENCODING",
            f"{name} contains multiple versions of one capability",
        )
    return struct.pack(">H", len(encoded_values)) + b"".join(
        struct.pack(">I", value) for value in encoded_values
    )


def decode_capabilities(encoded: bytes, name: str) -> List[int]:
    if len(encoded) < 2:
        fail("INVALID_LENGTH", f"{name} has no count")
    count = struct.unpack_from(">H", encoded)[0]
    if len(encoded) != 2 + count * 4:
        fail("INVALID_LENGTH", f"{name} length does not match its count")
    values = [
        struct.unpack_from(">I", encoded, 2 + index * 4)[0]
        for index in range(count)
    ]
    if values != sorted(set(values)):
        fail("NON_CANONICAL_ENCODING", f"{name} is not sorted and unique")
    identifiers = [value >> 16 for value in values]
    if len(set(identifiers)) != len(identifiers):
        fail(
            "NON_CANONICAL_ENCODING",
            f"{name} contains multiple versions of one capability",
        )
    return values


def encode_version_range(endpoint: Mapping[str, Any], name: str) -> bytes:
    minimum = require_sequence(endpoint.get("min_version"), f"{name}.min_version")
    maximum = require_sequence(endpoint.get("max_version"), f"{name}.max_version")
    if len(minimum) != 2 or len(maximum) != 2:
        fail("INVALID_MANIFEST", f"{name} versions must have two components")
    components = [
        require_integer(value, f"{name}.version", 0, 255)
        for value in (*minimum, *maximum)
    ]
    return bytes(components)


def decode_version_range(encoded: bytes, name: str) -> Tuple[Tuple[int, int], Tuple[int, int]]:
    minimum = (encoded[0], encoded[1])
    maximum = (encoded[2], encoded[3])
    if minimum[0] == 0 or maximum[0] == 0 or minimum > maximum:
        fail("INVALID_NEGOTIATION", f"{name} version range is invalid")
    return minimum, maximum


def encode_limits(value: Any, name: str) -> bytes:
    limits = require_mapping(value, name)
    return struct.pack(
        ">IIH",
        require_integer(limits.get("max_body"), f"{name}.max_body"),
        require_integer(
            limits.get("max_in_flight"), f"{name}.max_in_flight"
        ),
        require_integer(limits.get("max_streams"), f"{name}.max_streams", 0, 65535),
    )


def decode_limits(encoded: bytes) -> Tuple[int, int, int]:
    return struct.unpack(">IIH", encoded)


def encode_negotiation(value: Any) -> bytes:
    negotiation = require_mapping(value, "negotiation")
    initiator = require_mapping(negotiation.get("initiator"), "negotiation.initiator")
    responder = require_mapping(negotiation.get("responder"), "negotiation.responder")
    selected = require_mapping(negotiation.get("selected"), "negotiation.selected")
    selected_version = require_sequence(
        selected.get("version"), "negotiation.selected.version"
    )
    if len(selected_version) != 2:
        fail("INVALID_MANIFEST", "selected version must have two components")
    encoded = encode_object(
        KIND_NEGOTIATION,
        [
            (1, bytes([ROLE_INITIATOR])),
            (2, encode_version_range(initiator, "negotiation.initiator")),
            (
                3,
                encode_capabilities(
                    require_sequence(
                        initiator.get("capabilities"),
                        "negotiation.initiator.capabilities",
                    ),
                    "negotiation.initiator.capabilities",
                ),
            ),
            (
                4,
                encode_capabilities(
                    require_sequence(
                        initiator.get("required_capabilities"),
                        "negotiation.initiator.required_capabilities",
                    ),
                    "negotiation.initiator.required_capabilities",
                ),
            ),
            (
                5,
                encode_limits(
                    initiator.get("receive_limits"),
                    "negotiation.initiator.receive_limits",
                ),
            ),
            (6, bytes([ROLE_RESPONDER])),
            (7, encode_version_range(responder, "negotiation.responder")),
            (
                8,
                encode_capabilities(
                    require_sequence(
                        responder.get("capabilities"),
                        "negotiation.responder.capabilities",
                    ),
                    "negotiation.responder.capabilities",
                ),
            ),
            (
                9,
                encode_capabilities(
                    require_sequence(
                        responder.get("required_capabilities"),
                        "negotiation.responder.required_capabilities",
                    ),
                    "negotiation.responder.required_capabilities",
                ),
            ),
            (
                10,
                encode_limits(
                    responder.get("receive_limits"),
                    "negotiation.responder.receive_limits",
                ),
            ),
            (
                11,
                bytes(
                    require_integer(component, "negotiation.selected.version", 0, 255)
                    for component in selected_version
                ),
            ),
            (
                12,
                encode_capabilities(
                    require_sequence(
                        selected.get("capabilities"),
                        "negotiation.selected.capabilities",
                    ),
                    "negotiation.selected.capabilities",
                ),
            ),
            (
                13,
                encode_limits(
                    selected.get("effective_limits"),
                    "negotiation.selected.effective_limits",
                ),
            ),
        ],
    )
    parse_object(encoded, KIND_NEGOTIATION)
    return encoded


def validate_negotiation(fields: Mapping[int, bytes]) -> None:
    if fields[1] != bytes([ROLE_INITIATOR]) or fields[6] != bytes([ROLE_RESPONDER]):
        fail("ROLE_MISMATCH", "negotiation roles are not canonical")
    initiator_min, initiator_max = decode_version_range(fields[2], "initiator")
    responder_min, responder_max = decode_version_range(fields[7], "responder")
    common_minimum = max(initiator_min, responder_min)
    common_maximum = min(initiator_max, responder_max)
    if common_minimum > common_maximum:
        fail("UNSUPPORTED_VERSION", "version ranges do not intersect")
    selected_version = (fields[11][0], fields[11][1])
    if selected_version != common_maximum:
        fail(
            "DOWNGRADE_DETECTED",
            "selected version is not the highest common version",
        )

    initiator_caps = decode_capabilities(fields[3], "initiator capabilities")
    initiator_required = decode_capabilities(
        fields[4], "initiator required capabilities"
    )
    responder_caps = decode_capabilities(fields[8], "responder capabilities")
    responder_required = decode_capabilities(
        fields[9], "responder required capabilities"
    )
    selected_caps = decode_capabilities(fields[12], "selected capabilities")
    expected_caps = sorted(set(initiator_caps) & set(responder_caps))
    required = set(initiator_required) | set(responder_required)
    if (
        BASE_TRANSFER_V1 not in initiator_caps
        or BASE_TRANSFER_V1 not in responder_caps
        or BASE_TRANSFER_V1 not in required
    ):
        fail("UNSUPPORTED_CAPABILITY", "BASE_TRANSFER_V1 is not mandatory")
    if not required.issubset(expected_caps):
        fail("UNSUPPORTED_CAPABILITY", "a required capability is unavailable")
    if selected_caps != expected_caps:
        fail(
            "DOWNGRADE_DETECTED",
            "selected capabilities are not the exact intersection",
        )

    initiator_limits = decode_limits(fields[5])
    responder_limits = decode_limits(fields[10])
    expected_limits = tuple(
        min(left, right)
        for left, right in zip(initiator_limits, responder_limits)
    )
    if decode_limits(fields[13]) != expected_limits:
        fail(
            "DOWNGRADE_DETECTED",
            "effective limits are not the exact minima",
        )


def validate_raw_transcript(encoded: bytes) -> None:
    offset = 0
    tags: List[int] = []
    while offset < len(encoded):
        if len(encoded) - offset < 5:
            fail("MALFORMED_TRANSCRIPT", "raw record header is truncated")
        tag = encoded[offset]
        frame_length = struct.unpack_from(">I", encoded, offset + 1)[0]
        offset += 5
        if frame_length == 0 or frame_length > len(encoded) - offset:
            fail("MALFORMED_TRANSCRIPT", "raw record length is invalid")
        offset += frame_length
        tags.append(tag)
    if tags != [1, 2, 3, 4]:
        fail(
            "MALFORMED_TRANSCRIPT",
            "raw transcript tags are not 01,02,03,04 in role order",
        )


def validate_object(value: CanonicalObject) -> None:
    fields = value.fields
    if value.kind == KIND_NEGOTIATION:
        validate_negotiation(fields)
    elif value.kind == KIND_PAIR_CONTEXT:
        if fields[1] != PAIR_CONTEXT_LABEL:
            fail("DOMAIN_MISMATCH", "pairing context label is invalid")
        if fields[2] != bytes([ROLE_INITIATOR]) or fields[3] != bytes([ROLE_RESPONDER]):
            fail("ROLE_MISMATCH", "pairing roles are swapped or invalid")
        if struct.unpack(">H", fields[8])[0] != SECURITY_PROFILE_ID:
            fail("UNSUPPORTED_PROFILE", "security profile identifier is invalid")
        parse_object(fields[9], KIND_NEGOTIATION)
        validate_ed25519_public_key(fields[6], "pairing initiator public key")
        validate_ed25519_public_key(fields[7], "pairing responder public key")
    elif value.kind == KIND_SAS_INFO:
        if fields[1] != SAS_INFO_LABEL:
            fail("DOMAIN_MISMATCH", "SAS HKDF label is invalid")
    elif value.kind == KIND_TRANSPORT_CONTEXT:
        if fields[1] != TRANSPORT_CONTEXT_LABEL:
            fail("DOMAIN_MISMATCH", "transport context label is invalid")
        if fields[2] != bytes([ROLE_INITIATOR]) or fields[3] != bytes([ROLE_RESPONDER]):
            fail("ROLE_MISMATCH", "transport roles are swapped or invalid")
        if struct.unpack(">H", fields[8])[0] != SECURITY_PROFILE_ID:
            fail("UNSUPPORTED_PROFILE", "security profile identifier is invalid")
        parse_object(fields[9], KIND_NEGOTIATION)
        validate_raw_transcript(fields[10])
        validate_ed25519_public_key(fields[4], "transport initiator public key")
        validate_ed25519_public_key(fields[5], "transport responder public key")
    elif value.kind == KIND_ROTATION_CONTEXT:
        if fields[1] != ROTATION_CONTEXT_LABEL:
            fail("DOMAIN_MISMATCH", "rotation context label is invalid")
        if fields[2] == fields[3]:
            fail("INVALID_ROTATION", "old and new identity keys are identical")
        if struct.unpack(">Q", fields[4])[0] == 0:
            fail("INVALID_ROTATION", "rotation counter zero is invalid")
        validate_ed25519_public_key(fields[2], "rotation old public key")
        validate_ed25519_public_key(fields[3], "rotation new public key")
    elif value.kind == KIND_ROTATION_PROOF:
        if fields[1] != ROTATION_PROOF_LABEL:
            fail("DOMAIN_MISMATCH", "rotation proof label is invalid")
        if fields[3] not in {
            bytes([SIGNER_OLD_KEY]),
            bytes([SIGNER_NEW_KEY]),
        }:
            fail("ROLE_MISMATCH", "rotation signer is invalid")
    elif value.kind == KIND_DEVICE_IDENTIFIER:
        if fields[1] != DEVICE_IDENTIFIER_LABEL:
            fail("DOMAIN_MISMATCH", "device identifier label is invalid")
        validate_ed25519_public_key(fields[2], "device identifier public key")


def canonical_digest(encoded: bytes, expected_kind: int) -> bytes:
    parse_object(encoded, expected_kind)
    return hashlib.sha256(encoded).digest()


def fixture_hex(
    fixture: Mapping[str, Any], section: str, name: str, length: int
) -> bytes:
    values = require_mapping(fixture.get(section), f"fixture.{section}")
    return decode_hex(values.get(name), f"fixture.{section}.{name}", length)


def fixture_public_key(
    fixture: Mapping[str, Any], section: str, name: str
) -> bytes:
    public_key = fixture_hex(fixture, section, name, 32)
    validate_ed25519_public_key(public_key, f"fixture.{section}.{name}")
    return public_key


def profile_identifier(fixture: Mapping[str, Any]) -> bytes:
    value = require_integer(
        fixture.get("security_profile_id"),
        "fixture.security_profile_id",
        0,
        65535,
    )
    if value != SECURITY_PROFILE_ID:
        fail("UNSUPPORTED_PROFILE", f"profile {value} is unsupported")
    return struct.pack(">H", value)


def build_pair_context(fixture: Mapping[str, Any]) -> bytes:
    negotiation = encode_negotiation(fixture.get("negotiation"))
    return encode_object(
        KIND_PAIR_CONTEXT,
        [
            (1, PAIR_CONTEXT_LABEL),
            (2, bytes([ROLE_INITIATOR])),
            (3, bytes([ROLE_RESPONDER])),
            (4, fixture_hex(fixture, "pairing", "initiator_nonce_hex", 32)),
            (5, fixture_hex(fixture, "pairing", "responder_nonce_hex", 32)),
            (6, fixture_public_key(fixture, "identity", "initiator_key_hex")),
            (7, fixture_public_key(fixture, "identity", "responder_key_hex")),
            (8, profile_identifier(fixture)),
            (9, negotiation),
        ],
    )


def build_transport_context(fixture: Mapping[str, Any]) -> bytes:
    negotiation = encode_negotiation(fixture.get("negotiation"))
    transport = require_mapping(fixture.get("transport"), "fixture.transport")
    raw_transcript = decode_hex(
        transport.get("raw_negotiation_transcript_hex"),
        "fixture.transport.raw_negotiation_transcript_hex",
    )
    validate_raw_transcript(raw_transcript)
    return encode_object(
        KIND_TRANSPORT_CONTEXT,
        [
            (1, TRANSPORT_CONTEXT_LABEL),
            (2, bytes([ROLE_INITIATOR])),
            (3, bytes([ROLE_RESPONDER])),
            (4, fixture_public_key(fixture, "identity", "initiator_key_hex")),
            (5, fixture_public_key(fixture, "identity", "responder_key_hex")),
            (
                6,
                fixture_hex(
                    fixture, "transport", "initiator_session_nonce_hex", 32
                ),
            ),
            (
                7,
                fixture_hex(
                    fixture, "transport", "responder_session_nonce_hex", 32
                ),
            ),
            (8, profile_identifier(fixture)),
            (9, negotiation),
            (10, raw_transcript),
            (
                11,
                fixture_hex(fixture, "transport", "session_id_hex", 16),
            ),
        ],
    )


def build_rotation_context(fixture: Mapping[str, Any]) -> bytes:
    transport_context = canonical_digest(
        build_transport_context(fixture), KIND_TRANSPORT_CONTEXT
    )
    rotation = require_mapping(fixture.get("rotation"), "fixture.rotation")
    counter = require_integer(
        rotation.get("counter"), "fixture.rotation.counter", 1, 0xFFFFFFFFFFFFFFFF
    )
    return encode_object(
        KIND_ROTATION_CONTEXT,
        [
            (1, ROTATION_CONTEXT_LABEL),
            (2, fixture_public_key(fixture, "rotation", "old_key_hex")),
            (3, fixture_public_key(fixture, "rotation", "new_key_hex")),
            (4, struct.pack(">Q", counter)),
            (5, fixture_hex(fixture, "rotation", "nonce_hex", 32)),
            (6, transport_context),
        ],
    )


def build_rotation_proof(fixture: Mapping[str, Any], signer: int) -> bytes:
    rotation_context = canonical_digest(
        build_rotation_context(fixture), KIND_ROTATION_CONTEXT
    )
    return encode_object(
        KIND_ROTATION_PROOF,
        [
            (1, ROTATION_PROOF_LABEL),
            (2, rotation_context),
            (3, bytes([signer])),
        ],
    )


def build_device_identifier(public_key: bytes) -> bytes:
    validate_ed25519_public_key(public_key, "device identifier public key")
    encoded = encode_object(
        KIND_DEVICE_IDENTIFIER,
        [
            (1, DEVICE_IDENTIFIER_LABEL),
            (2, public_key),
        ],
    )
    parse_object(encoded, KIND_DEVICE_IDENTIFIER)
    return encoded


def derive_device_identifier(public_key: bytes) -> Dict[str, Any]:
    encoded = build_device_identifier(public_key)
    digest = hashlib.sha256(encoded).digest()
    return {
        "label_ascii": DEVICE_IDENTIFIER_LABEL.decode("ascii"),
        "label_hex": DEVICE_IDENTIFIER_LABEL.hex(),
        "public_key_hex": public_key.hex(),
        "canonical_input_hex": encoded.hex(),
        "sha256_hex": digest.hex(),
        "binary_length": len(digest),
        "text_encoding": "lowercase-hex-no-prefix",
    }


def hkdf_expand_sha256(prk: bytes, info: bytes, length: int) -> bytes:
    if len(prk) != hashlib.sha256().digest_size:
        fail("INVALID_LENGTH", "HKDF PRK must contain 32 octets")
    if length < 1 or length > 255 * hashlib.sha256().digest_size:
        fail("INVALID_LENGTH", "HKDF output length is invalid")
    output = bytearray()
    previous = b""
    counter = 1
    while len(output) < length:
        previous = hmac.new(
            prk, previous + info + bytes([counter]), hashlib.sha256
        ).digest()
        output.extend(previous)
        counter += 1
    return bytes(output[:length])


def derive_sas(
    exporter_material: bytes, pair_context: bytes, wordlist: Sequence[str]
) -> Dict[str, Any]:
    if len(exporter_material) != 32:
        fail("INVALID_LENGTH", "pairing exporter material must contain 32 octets")
    if len(pair_context) != 32:
        fail("INVALID_LENGTH", "pair context must contain 32 octets")
    info = encode_object(
        KIND_SAS_INFO,
        [(1, SAS_INFO_LABEL), (2, pair_context)],
    )
    parse_object(info, KIND_SAS_INFO)
    okm = hkdf_expand_sha256(exporter_material, info, 7)
    value = int.from_bytes(okm, "big") >> 1
    indices = [
        (value >> shift) & 0x7FF for shift in (44, 33, 22, 11, 0)
    ]
    return {
        "hkdf_info_hex": info.hex(),
        "hkdf_output_hex": okm.hex(),
        "indices": indices,
        "words": [wordlist[index] for index in indices],
    }


def confirmation_message(pair_context: bytes, role: int, decision: int) -> bytes:
    if len(pair_context) != 32:
        fail("INVALID_LENGTH", "pair context must contain 32 octets")
    if role not in {ROLE_INITIATOR, ROLE_RESPONDER}:
        fail("ROLE_MISMATCH", "confirmation role is invalid")
    if decision not in {DECISION_REJECT, DECISION_CONFIRM}:
        fail("INVALID_DECISION", "confirmation decision is invalid")
    return pair_context + bytes([role, decision])


def finished_message(transport_context: bytes, role: int) -> bytes:
    if len(transport_context) != 32:
        fail("INVALID_LENGTH", "transport context must contain 32 octets")
    if role not in {ROLE_INITIATOR, ROLE_RESPONDER}:
        fail("ROLE_MISMATCH", "transport-finished role is invalid")
    return transport_context + bytes([role])


def mac_sha256(key: bytes, message: bytes, name: str) -> bytes:
    if len(key) != 32:
        fail("INVALID_LENGTH", f"{name} key must contain 32 octets")
    return hmac.new(key, message, hashlib.sha256).digest()


def exporter_output(label: bytes, context: bytes) -> Dict[str, Any]:
    return {
        "label_ascii": label.decode("ascii"),
        "label_hex": label.hex(),
        "context_hex": context.hex(),
        "length": 32,
    }


def positive_output(
    operation: str,
    fixture: Mapping[str, Any],
    wordlist: Sequence[str],
) -> Dict[str, Any]:
    negotiation = encode_negotiation(fixture.get("negotiation"))
    pair_encoded = build_pair_context(fixture)
    pair_context = canonical_digest(pair_encoded, KIND_PAIR_CONTEXT)
    transport_encoded = build_transport_context(fixture)
    transport_context = canonical_digest(
        transport_encoded, KIND_TRANSPORT_CONTEXT
    )
    exporter = require_mapping(
        fixture.get("exporter_material"), "fixture.exporter_material"
    )

    if operation == "normalized_negotiation":
        return {
            "encoded_hex": negotiation.hex(),
            "sha256_hex": hashlib.sha256(negotiation).hexdigest(),
        }
    if operation == "pair_context":
        return {
            "encoded_hex": pair_encoded.hex(),
            "sha256_hex": pair_context.hex(),
        }
    if operation == "pairing_exporter_input":
        return exporter_output(PAIR_EXPORTER_LABEL, pair_context)
    if operation == "sas_words":
        material = decode_hex(
            exporter.get("pairing_hex"),
            "fixture.exporter_material.pairing_hex",
            32,
        )
        return derive_sas(material, pair_context, wordlist)
    if operation == "confirmation_exporter_input":
        return exporter_output(CONFIRMATION_EXPORTER_LABEL, pair_context)
    if operation in {"confirmation_initiator", "confirmation_responder"}:
        role = (
            ROLE_INITIATOR
            if operation == "confirmation_initiator"
            else ROLE_RESPONDER
        )
        key = decode_hex(
            exporter.get("confirmation_hex"),
            "fixture.exporter_material.confirmation_hex",
            32,
        )
        message = confirmation_message(pair_context, role, DECISION_CONFIRM)
        presented = mac_sha256(key, message, "confirmation exporter")
        result = require_trust_commit(
            key,
            message,
            presented,
            pair_context,
            role,
            DECISION_CONFIRM,
        )
        if (
            result.authenticated_result != AFFIRMATIVE_CONFIRM
            or not result.trust_commit_permitted
        ):
            fail(
                "OUTPUT_MISMATCH",
                "confirm vector did not satisfy the two-decision trust gate",
            )
        return {
            "message_hex": message.hex(),
            "hmac_sha256_hex": presented.hex(),
        }
    if operation in {
        "confirmation_reject_initiator",
        "confirmation_reject_responder",
    }:
        role = (
            ROLE_INITIATOR
            if operation == "confirmation_reject_initiator"
            else ROLE_RESPONDER
        )
        key = decode_hex(
            exporter.get("confirmation_hex"),
            "fixture.exporter_material.confirmation_hex",
            32,
        )
        message = confirmation_message(pair_context, role, DECISION_REJECT)
        presented = mac_sha256(key, message, "confirmation exporter")
        result = verify_confirmation(
            key,
            message,
            presented,
            pair_context,
            role,
        )
        return {
            "message_hex": message.hex(),
            "hmac_sha256_hex": presented.hex(),
            "sender_role": result.sender_role,
            "decision": result.decision,
            "authenticated_result": result.authenticated_result,
            "terminal": result.terminal,
            "trust_commit_permitted": result.trust_commit_permitted,
        }
    if operation == "device_identifier_initiator":
        public_key = fixture_public_key(
            fixture, "identity", "initiator_key_hex"
        )
        return derive_device_identifier(public_key)
    if operation == "transport_context":
        return {
            "encoded_hex": transport_encoded.hex(),
            "sha256_hex": transport_context.hex(),
        }
    if operation == "transport_exporter_input":
        return exporter_output(TRANSPORT_EXPORTER_LABEL, transport_context)
    if operation in {"transport_finished_initiator", "transport_finished_responder"}:
        role = (
            ROLE_INITIATOR
            if operation == "transport_finished_initiator"
            else ROLE_RESPONDER
        )
        key = decode_hex(
            exporter.get("transport_hex"),
            "fixture.exporter_material.transport_hex",
            32,
        )
        message = finished_message(transport_context, role)
        return {
            "message_hex": message.hex(),
            "hmac_sha256_hex": mac_sha256(
                key, message, "transport exporter"
            ).hex(),
        }
    if operation == "rotation_context":
        encoded = build_rotation_context(fixture)
        return {
            "encoded_hex": encoded.hex(),
            "sha256_hex": canonical_digest(
                encoded, KIND_ROTATION_CONTEXT
            ).hex(),
        }
    if operation in {"rotation_proof_old", "rotation_proof_new"}:
        signer = (
            SIGNER_OLD_KEY
            if operation == "rotation_proof_old"
            else SIGNER_NEW_KEY
        )
        encoded = build_rotation_proof(fixture, signer)
        return {
            "message_hex": encoded.hex(),
            "sha256_hex": hashlib.sha256(encoded).hexdigest(),
        }
    fail("INVALID_MANIFEST", f"unknown positive operation {operation!r}")


def verify_mac(
    key: bytes,
    message: bytes,
    presented: bytes,
    failure_code: str,
    name: str,
) -> None:
    expected = mac_sha256(key, message, name)
    if len(presented) != 32:
        fail("INVALID_LENGTH", f"{name} value must contain 32 octets")
    if not hmac.compare_digest(expected, presented):
        fail(failure_code, f"{name} value does not match")


def verify_confirmation(
    key: bytes,
    message: bytes,
    presented: bytes,
    expected_pair_context: bytes,
    expected_role: int,
) -> ConfirmationResult:
    if len(message) != 34:
        fail("INVALID_LENGTH", "confirmation message must contain 34 octets")
    if len(expected_pair_context) != 32:
        fail("INVALID_LENGTH", "expected pair context must contain 32 octets")
    if expected_role not in {ROLE_INITIATOR, ROLE_RESPONDER}:
        fail("ROLE_MISMATCH", "expected confirmation role is invalid")
    if message[32] != expected_role:
        fail("ROLE_MISMATCH", "confirmation role does not match the sender")
    decision = message[33]
    if decision not in {DECISION_REJECT, DECISION_CONFIRM}:
        fail("INVALID_DECISION", "confirmation decision is invalid")

    verify_mac(
        key,
        message,
        presented,
        "CONFIRMATION_MISMATCH",
        "confirmation",
    )
    if not hmac.compare_digest(message[:32], expected_pair_context):
        fail(
            "CONTEXT_MISMATCH",
            "confirmation does not bind the current live pair context",
        )

    if decision == DECISION_REJECT:
        return ConfirmationResult(
            sender_role=expected_role,
            decision=decision,
            authenticated_result=AUTHENTICATED_REJECT,
            terminal=True,
            trust_commit_permitted=False,
        )
    return ConfirmationResult(
        sender_role=expected_role,
        decision=decision,
        authenticated_result=AFFIRMATIVE_CONFIRM,
        terminal=False,
        trust_commit_permitted=False,
    )


def require_trust_commit(
    key: bytes,
    message: bytes,
    presented: bytes,
    expected_pair_context: bytes,
    expected_role: int,
    local_decision: int,
) -> ConfirmationResult:
    if local_decision not in {DECISION_REJECT, DECISION_CONFIRM}:
        fail("INVALID_DECISION", "local confirmation decision is invalid")
    if local_decision == DECISION_REJECT:
        fail("LOCAL_REJECT", "local rejection terminally closes the attempt")
    result = verify_confirmation(
        key,
        message,
        presented,
        expected_pair_context,
        expected_role,
    )
    if result.decision == DECISION_REJECT:
        fail(
            "AUTHENTICATED_REJECT",
            "peer authenticated rejection terminally closes the attempt",
        )
    return ConfirmationResult(
        sender_role=result.sender_role,
        decision=result.decision,
        authenticated_result=result.authenticated_result,
        terminal=False,
        trust_commit_permitted=True,
    )


def execute_negative(
    operation: str, inputs: Mapping[str, Any], wordlist: Sequence[str]
) -> None:
    if operation == "validate_ed25519_public_key":
        validate_ed25519_public_key(
            decode_hex(
                inputs.get("public_key_hex"),
                "input.public_key_hex",
            ),
            "input.public_key_hex",
        )
        return
    if operation == "decode_canonical":
        parse_object(decode_hex(inputs.get("encoded_hex"), "input.encoded_hex"))
        return
    if operation == "verify_context":
        encoded = decode_hex(inputs.get("encoded_hex"), "input.encoded_hex")
        expected_kind = require_integer(
            inputs.get("expected_kind"), "input.expected_kind", 1, 255
        )
        actual = canonical_digest(encoded, expected_kind)
        expected = decode_hex(
            inputs.get("expected_sha256_hex"),
            "input.expected_sha256_hex",
            32,
        )
        if not hmac.compare_digest(actual, expected):
            fail("CONTEXT_MISMATCH", "canonical context digest does not match")
        return
    if operation == "derive_sas":
        derive_sas(
            decode_hex(inputs.get("exporter_hex"), "input.exporter_hex"),
            decode_hex(
                inputs.get("pair_context_hex"), "input.pair_context_hex", 32
            ),
            wordlist,
        )
        return
    if operation == "verify_confirmation":
        verify_confirmation(
            decode_hex(inputs.get("key_hex"), "input.key_hex"),
            decode_hex(inputs.get("message_hex"), "input.message_hex"),
            decode_hex(inputs.get("presented_hex"), "input.presented_hex"),
            decode_hex(
                inputs.get("expected_pair_context_hex"),
                "input.expected_pair_context_hex",
                32,
            ),
            require_integer(
                inputs.get("expected_role"), "input.expected_role", 1, 2
            ),
        )
        return
    if operation == "require_trust_commit":
        require_trust_commit(
            decode_hex(inputs.get("key_hex"), "input.key_hex"),
            decode_hex(inputs.get("message_hex"), "input.message_hex"),
            decode_hex(inputs.get("presented_hex"), "input.presented_hex"),
            decode_hex(
                inputs.get("expected_pair_context_hex"),
                "input.expected_pair_context_hex",
                32,
            ),
            require_integer(
                inputs.get("expected_role"), "input.expected_role", 1, 2
            ),
            require_integer(
                inputs.get("local_decision"),
                "input.local_decision",
                DECISION_REJECT,
                DECISION_CONFIRM,
            ),
        )
        return
    if operation == "derive_device_identifier":
        encoded = decode_hex(
            inputs.get("canonical_input_hex"),
            "input.canonical_input_hex",
        )
        canonical_digest(encoded, KIND_DEVICE_IDENTIFIER)
        return
    if operation == "verify_device_identifier":
        public_key = decode_hex(
            inputs.get("public_key_hex"), "input.public_key_hex", 32
        )
        identifier_public_key = decode_hex(
            inputs.get("identifier_public_key_hex"),
            "input.identifier_public_key_hex",
            32,
        )
        validate_ed25519_public_key(public_key, "input.public_key_hex")
        validate_ed25519_public_key(
            identifier_public_key,
            "input.identifier_public_key_hex",
        )
        if hmac.compare_digest(public_key, identifier_public_key):
            fail(
                "INVALID_MANIFEST",
                "wrong-key vector must use two distinct public keys",
            )
        expected_public_key_identifier = decode_hex(
            inputs.get("public_key_sha256_hex"),
            "input.public_key_sha256_hex",
            32,
        )
        expected = decode_hex(
            inputs.get("expected_sha256_hex"),
            "input.expected_sha256_hex",
            32,
        )
        identifier = hashlib.sha256(
            build_device_identifier(identifier_public_key)
        ).digest()
        if not hmac.compare_digest(identifier, expected):
            fail(
                "OUTPUT_MISMATCH",
                "expected device identifier does not match its public key",
            )
        encoded = build_device_identifier(public_key)
        actual = hashlib.sha256(encoded).digest()
        if not hmac.compare_digest(actual, expected_public_key_identifier):
            fail(
                "OUTPUT_MISMATCH",
                "wrong-key device identifier does not match its public key",
            )
        if not hmac.compare_digest(actual, expected):
            fail(
                "DEVICE_IDENTIFIER_MISMATCH",
                "device identifier does not match the canonical public key",
            )
        return
    if operation == "verify_device_identifier_text":
        public_key = decode_hex(
            inputs.get("public_key_hex"), "input.public_key_hex", 32
        )
        presented = require_string(
            inputs.get("presented_text"), "input.presented_text"
        )
        if re.fullmatch(r"[0-9a-f]{64}", presented) is None:
            fail(
                "NON_CANONICAL_ENCODING",
                "device identifier text must be 64 lowercase hexadecimal characters",
            )
        encoded = build_device_identifier(public_key)
        actual = hashlib.sha256(encoded).hexdigest()
        if not hmac.compare_digest(actual, presented):
            fail(
                "DEVICE_IDENTIFIER_MISMATCH",
                "device identifier text does not match the canonical public key",
            )
        return
    if operation == "verify_transport_finished":
        key = decode_hex(inputs.get("key_hex"), "input.key_hex")
        message = decode_hex(inputs.get("message_hex"), "input.message_hex")
        if len(message) != 33:
            fail(
                "INVALID_LENGTH",
                "transport-finished message must contain 33 octets",
            )
        expected_role = require_integer(
            inputs.get("expected_role"), "input.expected_role", 1, 2
        )
        if message[32] != expected_role:
            fail(
                "ROLE_MISMATCH",
                "transport-finished role does not match the sender",
            )
        verify_mac(
            key,
            message,
            decode_hex(inputs.get("presented_hex"), "input.presented_hex"),
            "TRANSPORT_FINISHED_MISMATCH",
            "transport-finished",
        )
        return
    if operation == "validate_rotation_counter":
        previous = require_integer(
            inputs.get("previous"), "input.previous", 0, 0xFFFFFFFFFFFFFFFF
        )
        current = require_integer(
            inputs.get("current"), "input.current", 0, 0xFFFFFFFFFFFFFFFF
        )
        if current <= previous:
            fail("REPLAY_DETECTED", "rotation counter is not monotonic")
        return
    if operation == "verify_output":
        value = decode_hex(inputs.get("value_hex"), "input.value_hex")
        expected = decode_hex(
            inputs.get("expected_sha256_hex"),
            "input.expected_sha256_hex",
            32,
        )
        if not hmac.compare_digest(hashlib.sha256(value).digest(), expected):
            fail("OUTPUT_MISMATCH", "expected output digest does not match")
        return
    fail("INVALID_MANIFEST", f"unknown negative operation {operation!r}")


def pairing_schema(message_type: int) -> Dict[int, Tuple[int, Optional[int]]]:
    if message_type == PAIRING_HELLO:
        return {
            1: (PAIRING_WIRE_U8, 1),
            2: (PAIRING_WIRE_BYTES, 32),
            3: (PAIRING_WIRE_BYTES, 32),
            4: (PAIRING_WIRE_U16, 2),
            5: (PAIRING_WIRE_BYTES, 4),
            6: (PAIRING_WIRE_BYTES, None),
            7: (PAIRING_WIRE_BYTES, None),
            8: (PAIRING_WIRE_BYTES, 10),
        }
    if message_type in {PAIRING_SELECT, PAIRING_SELECT_ACK}:
        return {
            1: (PAIRING_WIRE_BYTES, 2),
            2: (PAIRING_WIRE_BYTES, None),
            3: (PAIRING_WIRE_BYTES, 10),
        }
    if message_type == PAIRING_DECISION:
        return {
            1: (PAIRING_WIRE_BYTES, 34),
            2: (PAIRING_WIRE_BYTES, 32),
        }
    if message_type == PAIRING_ABORT:
        return {1: (PAIRING_WIRE_U16, 2)}
    fail("PAIRING_MALFORMED", f"unknown pairing message type {message_type:#06x}")


def pairing_decode_value(wire_type: int, value: bytes) -> Any:
    if wire_type in {PAIRING_WIRE_U8, PAIRING_WIRE_U16}:
        return int.from_bytes(value, "big")
    return value


def parse_pairing_body(
    encoded: bytes,
    schema: Mapping[int, Tuple[int, Optional[int]]],
) -> Dict[int, Any]:
    offset = 0
    previous_id = 0
    fields: Dict[int, Any] = {}

    while offset < len(encoded):
        if len(encoded) - offset < 8:
            fail("PAIRING_MALFORMED", "trailing partial pairing TLV")
        field_id, wire_type, flags, value_length = struct.unpack_from(
            ">HBBI", encoded, offset
        )
        offset += 8
        if len(fields) >= PAIRING_MAX_FIELDS:
            fail("PAIRING_LIMIT_EXCEEDED", "pairing body has too many fields")
        if field_id <= previous_id:
            fail(
                "PAIRING_MALFORMED",
                "pairing fields are duplicate or out of order",
            )
        if flags != 0x01:
            fail("PAIRING_MALFORMED", "pairing field is not exactly critical")
        rule = schema.get(field_id)
        if rule is None:
            fail("PAIRING_MALFORMED", f"unknown pairing field {field_id}")
        expected_type, expected_length = rule
        if wire_type != expected_type:
            fail("PAIRING_MALFORMED", f"pairing field {field_id} has wrong type")
        if expected_length is not None and value_length != expected_length:
            fail(
                "PAIRING_MALFORMED",
                f"pairing field {field_id} has wrong length",
            )
        if value_length > len(encoded) - offset:
            fail("PAIRING_MALFORMED", f"pairing field {field_id} is truncated")
        value = encoded[offset : offset + value_length]
        offset += value_length
        fields[field_id] = pairing_decode_value(wire_type, value)
        previous_id = field_id

    if set(fields) != set(schema):
        missing = sorted(set(schema) - set(fields))
        fail("PAIRING_MALFORMED", f"pairing body is missing fields {missing}")
    return fields


def parse_pairing_frame(encoded: bytes) -> PairingFrame:
    if len(encoded) < PAIRING_FIXED_HEADER_LENGTH:
        fail("PAIRING_MALFORMED", "pairing fixed header is truncated")
    (
        magic,
        header_length,
        major,
        minor,
        message_type,
        flags,
        sequence,
        body_length,
    ) = struct.unpack_from(">4sHBBHHII", encoded)
    if magic != PAIRING_MAGIC:
        fail("PAIRING_MALFORMED", "pairing magic is invalid")
    if header_length != PAIRING_FIXED_HEADER_LENGTH or flags != 0:
        fail("PAIRING_MALFORMED", "pairing fixed header is noncanonical")
    if (major, minor) != (1, 0):
        fail("PAIRING_UNSUPPORTED_VERSION", "pairing frame version is unsupported")
    if sequence == 0:
        fail("PAIRING_SEQUENCE_VIOLATION", "pairing sequence zero is reserved")
    if body_length > PAIRING_MAX_BODY_LENGTH:
        fail("PAIRING_LIMIT_EXCEEDED", "pairing body exceeds the hard limit")
    total_length = PAIRING_FIXED_HEADER_LENGTH + body_length
    if total_length > PAIRING_MAX_FRAME_LENGTH:
        fail("PAIRING_LIMIT_EXCEEDED", "pairing frame exceeds the hard limit")
    if len(encoded) != total_length:
        fail("PAIRING_MALFORMED", "pairing encoded and declared lengths differ")
    fields = parse_pairing_body(
        encoded[PAIRING_FIXED_HEADER_LENGTH:],
        pairing_schema(message_type),
    )
    return PairingFrame(message_type, sequence, fields, len(encoded))


def pairing_decode_capabilities(encoded: bytes, name: str) -> List[int]:
    if len(encoded) < 2:
        fail("PAIRING_MALFORMED", f"{name} lacks a count")
    count = int.from_bytes(encoded[:2], "big")
    if count == 0 or len(encoded) != 2 + count * 4:
        fail("PAIRING_MALFORMED", f"{name} count and length disagree")
    values = [
        int.from_bytes(encoded[offset : offset + 4], "big")
        for offset in range(2, len(encoded), 4)
    ]
    if values != sorted(set(values)):
        fail("PAIRING_MALFORMED", f"{name} is not sorted and unique")
    if len({value >> 16 for value in values}) != len(values):
        fail("PAIRING_MALFORMED", f"{name} repeats a capability ID")
    return values


def pairing_decode_version_range(
    encoded: bytes,
) -> Tuple[Tuple[int, int], Tuple[int, int]]:
    if len(encoded) != 4:
        fail("PAIRING_MALFORMED", "version range must contain four octets")
    minimum = (encoded[0], encoded[1])
    maximum = (encoded[2], encoded[3])
    if minimum[0] == 0 or minimum > maximum:
        fail("PAIRING_MALFORMED", "version range is invalid")
    return minimum, maximum


def pairing_decode_limits(encoded: bytes) -> Tuple[int, int, int]:
    if len(encoded) != 10:
        fail("PAIRING_MALFORMED", "receive limits must contain ten octets")
    maximum_body, maximum_in_flight, maximum_streams = struct.unpack(
        ">IIH", encoded
    )
    if not 8_192 <= maximum_body <= 1_048_576:
        fail("PAIRING_LIMIT_EXCEEDED", "receive maximum body is invalid")
    if not 1_048_576 <= maximum_in_flight <= 67_108_864:
        fail("PAIRING_LIMIT_EXCEEDED", "receive in-flight limit is invalid")
    if not 1 <= maximum_streams <= 32:
        fail("PAIRING_LIMIT_EXCEEDED", "receive stream limit is invalid")
    return maximum_body, maximum_in_flight, maximum_streams


class PairingTranscriptValidator:
    def __init__(
        self,
        initiator_key: bytes,
        responder_key: bytes,
        confirmation_exporter: bytes,
    ) -> None:
        self.identity_keys = {
            PAIRING_INITIATOR_TO_RESPONDER: initiator_key,
            PAIRING_RESPONDER_TO_INITIATOR: responder_key,
        }
        self.confirmation_exporter = confirmation_exporter
        self.next_sequence = {
            PAIRING_INITIATOR_TO_RESPONDER: 1,
            PAIRING_RESPONDER_TO_INITIATOR: 1,
        }
        self.message_counts = {
            PAIRING_INITIATOR_TO_RESPONDER: 0,
            PAIRING_RESPONDER_TO_INITIATOR: 0,
        }
        self.byte_counts = {
            PAIRING_INITIATOR_TO_RESPONDER: 0,
            PAIRING_RESPONDER_TO_INITIATOR: 0,
        }
        self.hellos: Dict[str, PairingFrame] = {}
        self.selection: Optional[PairingFrame] = None
        self.pair_context: Optional[bytes] = None
        self.decisions: Dict[str, int] = {}
        self.commit_ready = False
        self.terminal: Optional[str] = None

    def process(self, direction: str, encoded: bytes) -> None:
        if self.terminal is not None:
            fail("PAIRING_STATE_VIOLATION", "pairing input followed terminal state")
        if direction not in PAIRING_DIRECTIONS:
            fail("PAIRING_MALFORMED", f"invalid pairing direction {direction!r}")
        frame = parse_pairing_frame(encoded)
        if frame.sequence != self.next_sequence[direction]:
            fail(
                "PAIRING_SEQUENCE_VIOLATION",
                f"expected sequence {self.next_sequence[direction]}, "
                f"got {frame.sequence}",
            )
        self.next_sequence[direction] += 1
        self.message_counts[direction] += 1
        self.byte_counts[direction] += frame.encoded_length
        if self.message_counts[direction] > PAIRING_MAX_MESSAGES:
            fail("PAIRING_LIMIT_EXCEEDED", "pairing message budget exhausted")
        if self.byte_counts[direction] > PAIRING_MAX_BYTES:
            fail("PAIRING_LIMIT_EXCEEDED", "pairing byte budget exhausted")

        if frame.message_type == PAIRING_HELLO:
            self.process_hello(direction, frame)
        elif frame.message_type == PAIRING_SELECT:
            self.process_select(direction, frame)
        elif frame.message_type == PAIRING_SELECT_ACK:
            self.process_select_ack(direction, frame)
        elif frame.message_type == PAIRING_DECISION:
            self.process_decision(direction, frame)
        elif frame.message_type == PAIRING_ABORT:
            public_code = frame.fields[1]
            if public_code not in {1, 2, 3, 4}:
                fail("PAIRING_MALFORMED", "pairing abort code is invalid")
            self.terminal = "aborted"

    def process_hello(self, direction: str, frame: PairingFrame) -> None:
        if self.selection is not None or direction in self.hellos:
            fail("PAIRING_STATE_VIOLATION", "pairing hello is duplicate or late")
        expected_role = (
            ROLE_INITIATOR
            if direction == PAIRING_INITIATOR_TO_RESPONDER
            else ROLE_RESPONDER
        )
        if frame.fields[1] != expected_role:
            fail("PAIRING_ROLE_MISMATCH", "hello role conflicts with TLS direction")
        if frame.fields[3] != self.identity_keys[direction]:
            fail(
                "PAIRING_CERTIFICATE_REJECTED",
                "hello identity differs from the live certificate",
            )
        try:
            validate_ed25519_public_key(frame.fields[3], "pairing identity key")
        except VectorError:
            fail(
                "PAIRING_CERTIFICATE_REJECTED",
                "hello identity key is not a valid profile key",
            )
        if any(
            other.fields[3] == frame.fields[3] for other in self.hellos.values()
        ):
            fail(
                "PAIRING_CERTIFICATE_REJECTED",
                "local and remote pairing identities are equal",
            )
        if frame.fields[4] != SECURITY_PROFILE_ID:
            fail("PAIRING_UNSUPPORTED_PROFILE", "security profile is not 0001")
        pairing_decode_version_range(frame.fields[5])
        offered = pairing_decode_capabilities(
            frame.fields[6], "offered capabilities"
        )
        required = pairing_decode_capabilities(
            frame.fields[7], "required capabilities"
        )
        if not set(required).issubset(offered) or BASE_TRANSFER_V1 not in required:
            fail(
                "PAIRING_MALFORMED",
                "required capabilities are not an offered profile",
            )
        pairing_decode_limits(frame.fields[8])
        self.hellos[direction] = frame

    def expected_selection(self) -> Tuple[bytes, bytes, bytes]:
        if set(self.hellos) != PAIRING_DIRECTIONS:
            fail("PAIRING_STATE_VIOLATION", "selection arrived before both hellos")
        initiator = self.hellos[PAIRING_INITIATOR_TO_RESPONDER].fields
        responder = self.hellos[PAIRING_RESPONDER_TO_INITIATOR].fields
        initiator_min, initiator_max = pairing_decode_version_range(initiator[5])
        responder_min, responder_max = pairing_decode_version_range(responder[5])
        minimum = max(initiator_min, responder_min)
        maximum = min(initiator_max, responder_max)
        if minimum > maximum:
            fail("PAIRING_UNSUPPORTED_VERSION", "version ranges do not intersect")
        offered = sorted(
            set(pairing_decode_capabilities(initiator[6], "initiator offered"))
            & set(pairing_decode_capabilities(responder[6], "responder offered"))
        )
        required = set(
            pairing_decode_capabilities(initiator[7], "initiator required")
        ) | set(pairing_decode_capabilities(responder[7], "responder required"))
        if not required.issubset(offered):
            fail("PAIRING_MALFORMED", "required capability is unavailable")
        capability_bytes = struct.pack(">H", len(offered)) + b"".join(
            struct.pack(">I", value) for value in offered
        )
        initiator_limits = pairing_decode_limits(initiator[8])
        responder_limits = pairing_decode_limits(responder[8])
        limits = tuple(
            min(left, right)
            for left, right in zip(initiator_limits, responder_limits)
        )
        return (
            bytes(maximum),
            capability_bytes,
            struct.pack(">IIH", *limits),
        )

    def process_select(self, direction: str, frame: PairingFrame) -> None:
        if direction != PAIRING_INITIATOR_TO_RESPONDER:
            fail("PAIRING_ROLE_MISMATCH", "responder sent pairing selection")
        if self.selection is not None:
            fail("PAIRING_STATE_VIOLATION", "pairing selection is duplicated")
        expected_version, expected_capabilities, expected_limits = (
            self.expected_selection()
        )
        if frame.fields[1] != expected_version:
            fail(
                "PAIRING_DOWNGRADE_DETECTED",
                "selection is not the highest common version",
            )
        if (
            frame.fields[2] != expected_capabilities
            or frame.fields[3] != expected_limits
        ):
            fail("PAIRING_MALFORMED", "pairing selection is not deterministic")
        self.selection = frame

    def process_select_ack(self, direction: str, frame: PairingFrame) -> None:
        if direction != PAIRING_RESPONDER_TO_INITIATOR:
            fail("PAIRING_ROLE_MISMATCH", "initiator sent pairing selection ACK")
        if self.selection is None:
            fail("PAIRING_STATE_VIOLATION", "selection ACK arrived before selection")
        if self.pair_context is not None:
            fail("PAIRING_STATE_VIOLATION", "selection ACK is duplicated")
        if frame.fields != self.selection.fields:
            fail("PAIRING_MALFORMED", "selection ACK differs from selection")
        self.pair_context = self.build_pair_context()

    def build_pair_context(self) -> bytes:
        initiator = self.hellos[PAIRING_INITIATOR_TO_RESPONDER].fields
        responder = self.hellos[PAIRING_RESPONDER_TO_INITIATOR].fields
        if self.selection is None:
            fail("PAIRING_STATE_VIOLATION", "pairing selection is absent")
        normalized = encode_object(
            KIND_NEGOTIATION,
            (
                (1, bytes([ROLE_INITIATOR])),
                (2, initiator[5]),
                (3, initiator[6]),
                (4, initiator[7]),
                (5, initiator[8]),
                (6, bytes([ROLE_RESPONDER])),
                (7, responder[5]),
                (8, responder[6]),
                (9, responder[7]),
                (10, responder[8]),
                (11, self.selection.fields[1]),
                (12, self.selection.fields[2]),
                (13, self.selection.fields[3]),
            ),
        )
        pairing_context = encode_object(
            KIND_PAIR_CONTEXT,
            (
                (1, PAIR_CONTEXT_LABEL),
                (2, bytes([ROLE_INITIATOR])),
                (3, bytes([ROLE_RESPONDER])),
                (4, initiator[2]),
                (5, responder[2]),
                (6, initiator[3]),
                (7, responder[3]),
                (8, struct.pack(">H", SECURITY_PROFILE_ID)),
                (9, normalized),
            ),
        )
        return hashlib.sha256(pairing_context).digest()

    def process_decision(self, direction: str, frame: PairingFrame) -> None:
        if self.pair_context is None:
            fail("PAIRING_STATE_VIOLATION", "decision arrived before selection ACK")
        if direction in self.decisions:
            fail("PAIRING_STATE_VIOLATION", "pairing decision is duplicated")
        expected_role = (
            ROLE_INITIATOR
            if direction == PAIRING_INITIATOR_TO_RESPONDER
            else ROLE_RESPONDER
        )
        message = frame.fields[1]
        if not hmac.compare_digest(message[:32], self.pair_context):
            fail(
                "PAIRING_CONFIRMATION_FAILED",
                "decision context differs from the live attempt",
            )
        if message[32] != expected_role:
            fail("PAIRING_ROLE_MISMATCH", "decision role conflicts with direction")
        decision = message[33]
        if decision not in {DECISION_REJECT, DECISION_CONFIRM}:
            fail("PAIRING_MALFORMED", "pairing decision is invalid")
        expected = hmac.new(
            self.confirmation_exporter,
            message,
            hashlib.sha256,
        ).digest()
        if not hmac.compare_digest(frame.fields[2], expected):
            fail(
                "PAIRING_CONFIRMATION_FAILED",
                "pairing confirmation does not bind the live attempt",
            )
        self.decisions[direction] = decision
        if decision == DECISION_REJECT:
            self.terminal = "rejected"
        elif set(self.decisions) == PAIRING_DIRECTIONS:
            self.commit_ready = True


def load_pairing_frame(
    frame_catalog: Mapping[str, Any],
    frame_name: Any,
) -> Tuple[str, bytes]:
    if not isinstance(frame_name, str):
        fail("INVALID_MANIFEST", "pairing frame reference must be a string")
    raw_frame = require_mapping(frame_catalog.get(frame_name), frame_name)
    direction = require_string(raw_frame.get("direction"), f"{frame_name}.direction")
    if direction not in PAIRING_DIRECTIONS:
        fail("INVALID_MANIFEST", f"{frame_name}.direction is invalid")
    return direction, decode_hex(raw_frame.get("hex"), f"{frame_name}.hex")


def run_pairing_case(
    case: Mapping[str, Any],
    frame_catalog: Mapping[str, Any],
    fixtures: Mapping[str, Any],
) -> str:
    mode = require_string(case.get("mode"), "case.mode")
    alpn = decode_hex(case.get("alpn_hex"), "case.alpn_hex")
    if mode == "pairing":
        if alpn != PAIRING_ALPN:
            fail("PAIRING_UNSUPPORTED_VERSION", "pairing ALPN is not registered")
        fixture_name = require_string(case.get("fixture"), "case.fixture")
        fixture = require_mapping(fixtures.get(fixture_name), f"fixtures.{fixture_name}")
        validator = PairingTranscriptValidator(
            decode_hex(
                fixture.get("initiator_identity_key_hex"),
                f"{fixture_name}.initiator_identity_key_hex",
                32,
            ),
            decode_hex(
                fixture.get("responder_identity_key_hex"),
                f"{fixture_name}.responder_identity_key_hex",
                32,
            ),
            decode_hex(
                fixture.get("confirmation_exporter_hex"),
                f"{fixture_name}.confirmation_exporter_hex",
                32,
            ),
        )
        for frame_name in require_sequence(case.get("frames"), "case.frames"):
            direction, encoded = load_pairing_frame(frame_catalog, frame_name)
            validator.process(direction, encoded)
        expected_context = case.get("expected_pair_context_hex")
        if expected_context is not None:
            expected = decode_hex(
                expected_context, "case.expected_pair_context_hex", 32
            )
            if validator.pair_context is None or not hmac.compare_digest(
                validator.pair_context, expected
            ):
                fail(
                    "PAIRING_CONFIRMATION_FAILED",
                    "pairing context does not match the normative digest",
                )
        local_commit = case.get("local_commit")
        if validator.commit_ready:
            outcome = require_string(local_commit, "case.local_commit")
            if outcome == "success":
                validator.terminal = "paired_local"
            elif outcome == "failure":
                fail(
                    "PAIRING_INTERNAL_FAILURE",
                    "atomic local trust commit failed",
                )
            elif outcome == "cancelled_before_invoke":
                fail(
                    "PAIRING_CANCELLED",
                    "local cancellation won before trust commit invocation",
                )
            else:
                fail("INVALID_MANIFEST", "case.local_commit is invalid")
        elif local_commit is not None:
            fail(
                "INVALID_MANIFEST",
                "case.local_commit is present before both confirmations",
            )
        if validator.terminal is None:
            fail("PAIRING_STATE_VIOLATION", "pairing case is not terminal")
        return validator.terminal
    if mode == "reconnect":
        if alpn != TRANSPORT_ALPN:
            fail("PAIRING_UNSUPPORTED_VERSION", "transport ALPN is not registered")
        if require_integer(
            case.get("security_profile"), "case.security_profile", 0, 0xFFFF
        ) != SECURITY_PROFILE_ID:
            fail("PAIRING_UNSUPPORTED_PROFILE", "reconnect profile is not 0001")
        if require_string(case.get("trust_state"), "case.trust_state") != "active":
            fail("PAIRING_CERTIFICATE_REJECTED", "peer trust state is not active")
        presented = decode_hex(
            case.get("presented_key_hex"), "case.presented_key_hex", 32
        )
        pinned = decode_hex(case.get("active_pin_hex"), "case.active_pin_hex", 32)
        validate_ed25519_public_key(presented, "presented reconnect key")
        validate_ed25519_public_key(pinned, "stored reconnect pin")
        if not hmac.compare_digest(presented, pinned):
            fail("PAIRING_CERTIFICATE_REJECTED", "reconnect key differs from pin")
        stored_floor = require_integer(
            case.get("stored_security_floor"),
            "case.stored_security_floor",
            1,
            0xFFFF,
        )
        if SECURITY_PROFILE_ID < stored_floor:
            fail(
                "PAIRING_UNSUPPORTED_PROFILE",
                "reconnect profile is below the stored security floor",
            )
        if case.get("fresh_handshake") is not True:
            fail("PAIRING_REPLAY_DETECTED", "reconnect handshake is not fresh")
        if case.get("session_reused") is not False:
            fail("PAIRING_REPLAY_DETECTED", "TLS session reuse is forbidden")
        if case.get("early_data") is not False:
            fail("PAIRING_STATE_VIOLATION", "TLS early data is forbidden")
        binding = require_mapping(
            case.get("transport_binding"), "case.transport_binding"
        )
        if binding != PAIRING_EXPECTED_RECONNECT:
            fail(
                "PAIRING_CONFIRMATION_FAILED",
                "reconnect transport binding differs from the golden profile",
            )
        context = decode_hex(
            binding.get("transport_context_hex"),
            "transport_binding.transport_context_hex",
            32,
        )
        exporter = decode_hex(
            binding.get("transport_exporter_hex"),
            "transport_binding.transport_exporter_hex",
            32,
        )
        for role, field in (
            (ROLE_INITIATOR, "initiator_finished_hex"),
            (ROLE_RESPONDER, "responder_finished_hex"),
        ):
            expected_finished = hmac.new(
                exporter,
                context + bytes([role]),
                hashlib.sha256,
            ).digest()
            presented_finished = decode_hex(
                binding.get(field),
                f"transport_binding.{field}",
                32,
            )
            if not hmac.compare_digest(expected_finished, presented_finished):
                fail(
                    "PAIRING_CONFIRMATION_FAILED",
                    f"{field} does not verify",
                )
        return "established"
    fail("INVALID_MANIFEST", f"unknown pairing vector mode {mode!r}")


def validate_pairing_manifest(manifest: Mapping[str, Any]) -> int:
    if manifest.get("format_version") != 1:
        fail("INVALID_MANIFEST", "unsupported pairing vector format")
    if manifest.get("contract_type") != "xnn-pairing-control-v1":
        fail("INVALID_MANIFEST", "pairing vector contract type is invalid")
    contract = require_mapping(manifest.get("contract"), "contract")
    if contract != PAIRING_EXPECTED_CONTRACT:
        fail("INVALID_MANIFEST", "pairing contract metadata is not byte-exact")
    fixtures = require_mapping(manifest.get("fixtures"), "fixtures")
    frame_catalog = require_mapping(manifest.get("frames"), "frames")
    cases = require_sequence(manifest.get("cases"), "cases")
    names: set = set()
    coverage: set = set()
    referenced_frames: set = set()
    failures = 0
    positive = 0
    hostile = 0

    for raw_case in cases:
        try:
            case = require_mapping(raw_case, "pairing case")
            name = require_string(case.get("id"), "case.id")
            if name in names:
                fail("INVALID_MANIFEST", f"duplicate pairing case {name!r}")
            names.add(name)
            category = require_string(case.get("coverage"), f"{name}.coverage")
            coverage.add(category)
            classification = require_string(
                case.get("classification"), f"{name}.classification"
            )
            if classification not in {"positive", "hostile"}:
                fail("INVALID_MANIFEST", f"{name}.classification is invalid")
            expected = require_mapping(case.get("expect"), f"{name}.expect")
            expected_result = require_string(
                expected.get("result"), f"{name}.expect.result"
            )
            if (classification == "positive") != (expected_result == "accept"):
                fail(
                    "INVALID_MANIFEST",
                    f"{name}.classification conflicts with its expected result",
                )
            if case.get("mode") == "pairing":
                for frame_name in require_sequence(
                    case.get("frames"), f"{name}.frames"
                ):
                    if not isinstance(frame_name, str):
                        fail(
                            "INVALID_MANIFEST",
                            f"{name}.frames contains a non-string reference",
                        )
                    referenced_frames.add(frame_name)
            actual_error: Optional[VectorError] = None
            actual_terminal: Optional[str] = None
            try:
                actual_terminal = run_pairing_case(case, frame_catalog, fixtures)
            except VectorError as error:
                actual_error = error

            if expected_result == "accept":
                positive += 1
                expected_terminal = require_string(
                    expected.get("terminal"), f"{name}.expect.terminal"
                )
                if actual_error is not None:
                    fail(
                        "OUTPUT_MISMATCH",
                        f"{name} unexpectedly failed: {actual_error}",
                    )
                if actual_terminal != expected_terminal:
                    fail(
                        "OUTPUT_MISMATCH",
                        f"{name} expected {expected_terminal}, "
                        f"got {actual_terminal}",
                    )
                print(f"[PASS] {name}: {actual_terminal}")
            elif expected_result == "reject":
                hostile += 1
                expected_error = require_string(
                    expected.get("error"), f"{name}.expect.error"
                )
                if actual_error is None:
                    fail(
                        "OUTPUT_MISMATCH",
                        f"{name} expected {expected_error}, but was accepted",
                    )
                if actual_error.code != expected_error:
                    fail(
                        "OUTPUT_MISMATCH",
                        f"{name} expected {expected_error}, "
                        f"got {actual_error.code}",
                    )
                print(f"[PASS] {name}: {actual_error.code}")
            else:
                fail("INVALID_MANIFEST", f"{name}.expect.result is invalid")
        except VectorError as error:
            failures += 1
            print(f"[FAIL] pairing vector: {error}")

    missing_coverage = PAIRING_REQUIRED_COVERAGE - coverage
    if missing_coverage:
        failures += 1
        print(
            "[FAIL] pairing vector: missing required coverage "
            + ", ".join(sorted(missing_coverage))
        )
    unused_frames = set(frame_catalog) - referenced_frames
    unknown_frames = referenced_frames - set(frame_catalog)
    if unused_frames or unknown_frames:
        failures += 1
        print(
            "[FAIL] pairing vector: frame catalog mismatch; "
            f"unused={sorted(unused_frames)}, unknown={sorted(unknown_frames)}"
        )
    if failures:
        print(
            f"{failures} of {len(cases)} pairing-control vector checks failed.",
            file=sys.stderr,
        )
        return 1
    print(
        f"Validated {positive} positive and {hostile} hostile "
        "XnnTransfer v1 pairing-control vectors."
    )
    return 0


def validate_case_metadata(case: Mapping[str, Any], names: set) -> str:
    name = require_string(case.get("id"), "case.id")
    if name in names:
        fail("INVALID_MANIFEST", f"duplicate vector id {name!r}")
    names.add(name)
    require_string(case.get("description"), f"{name}.description")
    require_string(case.get("input_encoding"), f"{name}.input_encoding")
    invariants = require_sequence(case.get("invariants"), f"{name}.invariants")
    if not invariants or not all(
        isinstance(invariant, str) and invariant for invariant in invariants
    ):
        fail("INVALID_MANIFEST", f"{name}.invariants must be nonempty strings")
    return name


def canonical_wordlist_bytes(encoded: bytes) -> bytes:
    canonical = encoded.replace(b"\r\n", b"\n")
    if b"\r" in canonical:
        fail("WORDLIST_MISMATCH", "word list contains a bare carriage return")
    return canonical


def load_wordlist(path: Path, expected_sha256: str) -> List[str]:
    encoded = canonical_wordlist_bytes(path.read_bytes())
    if hashlib.sha256(encoded).hexdigest() != expected_sha256:
        fail("WORDLIST_MISMATCH", "word-list SHA-256 does not match the manifest")
    try:
        text = encoded.decode("ascii")
    except UnicodeDecodeError:
        fail("WORDLIST_MISMATCH", "word list is not ASCII")
    words = text.splitlines()
    if len(words) != WORD_COUNT:
        fail(
            "WORDLIST_MISMATCH",
            f"word list must contain {WORD_COUNT} entries, got {len(words)}",
        )
    if words != sorted(set(words)):
        fail("WORDLIST_MISMATCH", "word list is not sorted and unique")
    if any(re.fullmatch(r"[a-z]+", word) is None for word in words):
        fail("WORDLIST_MISMATCH", "word list contains a non-lowercase-ASCII word")
    return words


def validate_manifest(manifest: Mapping[str, Any], manifest_path: Path) -> int:
    if manifest.get("format_version") != 1:
        fail("INVALID_MANIFEST", "unsupported vector manifest format")
    profile = require_mapping(manifest.get("profile"), "profile")
    for key, expected in EXPECTED_PROFILE_METADATA.items():
        if profile.get(key) != expected:
            fail(
                "INVALID_MANIFEST",
                f"profile.{key} must equal {expected!r}",
            )
    subgroup_evidence = require_mapping(
        manifest.get("ed25519_subgroup_evidence"),
        "ed25519_subgroup_evidence",
    )
    if subgroup_evidence != EXPECTED_SUBGROUP_EVIDENCE:
        fail(
            "INVALID_MANIFEST",
            "ed25519_subgroup_evidence does not match the reviewed BR-04 "
            "evidence",
        )
    expected_wordlist_sha = require_string(
        profile.get("wordlist_sha256"), "profile.wordlist_sha256"
    )
    wordlist = load_wordlist(
        manifest_path.with_name("wordlist.txt"), expected_wordlist_sha
    )

    fixtures = require_mapping(manifest.get("fixtures"), "fixtures")
    for fixture_name, raw_fixture in fixtures.items():
        fixture = require_mapping(raw_fixture, f"fixtures.{fixture_name}")
        provenance = require_mapping(
            fixture.get("key_provenance"),
            f"fixtures.{fixture_name}.key_provenance",
        )
        for field in (
            "source",
            "source_url",
            "identity_initiator",
            "identity_responder",
            "rotation_old",
            "rotation_new",
        ):
            require_string(
                provenance.get(field),
                f"fixtures.{fixture_name}.key_provenance.{field}",
            )
        fixture_public_key(fixture, "identity", "initiator_key_hex")
        fixture_public_key(fixture, "identity", "responder_key_hex")
        fixture_public_key(fixture, "rotation", "old_key_hex")
        fixture_public_key(fixture, "rotation", "new_key_hex")
    positive = require_sequence(manifest.get("vectors"), "vectors")
    negative = require_sequence(
        manifest.get("negative_vectors"), "negative_vectors"
    )
    names: set = set()
    failures = 0

    for raw_case in positive:
        try:
            case = require_mapping(raw_case, "vector")
            name = validate_case_metadata(case, names)
            if require_mapping(case.get("expect"), f"{name}.expect").get("result") != "accept":
                fail("INVALID_MANIFEST", f"{name} must expect accept")
            fixture_name = require_string(case.get("fixture"), f"{name}.fixture")
            fixture = require_mapping(
                fixtures.get(fixture_name), f"fixtures.{fixture_name}"
            )
            operation = require_string(case.get("operation"), f"{name}.operation")
            actual = positive_output(operation, fixture, wordlist)
            expected = require_mapping(
                require_mapping(case.get("expect"), f"{name}.expect").get(
                    "output"
                ),
                f"{name}.expect.output",
            )
            if actual != expected:
                fail(
                    "OUTPUT_MISMATCH",
                    f"expected {dict(expected)!r}, got {actual!r}",
                )
            print(f"[PASS] {name}: accept")
        except (KeyError, VectorError) as error:
            failures += 1
            print(f"[FAIL] positive vector: {error}")

    for raw_case in negative:
        try:
            case = require_mapping(raw_case, "negative vector")
            name = validate_case_metadata(case, names)
            expect = require_mapping(case.get("expect"), f"{name}.expect")
            if expect.get("result") != "reject":
                fail("INVALID_MANIFEST", f"{name} must expect reject")
            expected_error = require_string(expect.get("error"), f"{name}.expect.error")
            operation = require_string(case.get("operation"), f"{name}.operation")
            inputs = require_mapping(case.get("input"), f"{name}.input")
            actual_error: Optional[VectorError] = None
            try:
                execute_negative(operation, inputs, wordlist)
            except VectorError as error:
                actual_error = error
            if actual_error is None:
                fail(
                    "OUTPUT_MISMATCH",
                    f"expected {expected_error}, but input was accepted",
                )
            if actual_error.code != expected_error:
                fail(
                    "OUTPUT_MISMATCH",
                    f"expected {expected_error}, got {actual_error.code}",
                )
            print(f"[PASS] {name}: {actual_error.code}")
        except (KeyError, VectorError) as error:
            failures += 1
            print(f"[FAIL] negative vector: {error}")

    total = len(positive) + len(negative)
    if failures:
        print(f"{failures} of {total} security vector cases failed.", file=sys.stderr)
        return 1
    print(
        f"Validated {len(positive)} positive and {len(negative)} negative "
        "XnnTransfer v1 security-profile vectors."
    )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "manifest",
        nargs="?",
        type=Path,
    )
    args = parser.parse_args()
    try:
        if args.manifest is not None:
            with args.manifest.open("r", encoding="utf-8") as source:
                manifest = require_mapping(json.load(source), "manifest")
            if manifest.get("contract_type") == "xnn-pairing-control-v1":
                return validate_pairing_manifest(manifest)
            return validate_manifest(manifest, args.manifest)

        fixture_root = Path(__file__).resolve().parent
        profile_path = fixture_root / "vectors.json"
        pairing_path = fixture_root / "pairing-control-vectors.json"
        with profile_path.open("r", encoding="utf-8") as source:
            profile_manifest = require_mapping(json.load(source), "manifest")
        with pairing_path.open("r", encoding="utf-8") as source:
            pairing_manifest = require_mapping(json.load(source), "manifest")
        profile_result = validate_manifest(profile_manifest, profile_path)
        pairing_result = validate_pairing_manifest(pairing_manifest)
        return 1 if profile_result or pairing_result else 0
    except (OSError, json.JSONDecodeError, VectorError) as error:
        print(f"security vector validation failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
