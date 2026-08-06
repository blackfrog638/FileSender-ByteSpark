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
    "device_identifier_label_hex": DEVICE_IDENTIFIER_LABEL.hex(),
    "device_identifier_input": "XNNS-canonical-kind-07",
    "device_identifier_hash": "SHA-256",
    "device_identifier_text": "lowercase-hex-no-prefix",
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
    elif value.kind == KIND_ROTATION_CONTEXT:
        if fields[1] != ROTATION_CONTEXT_LABEL:
            fail("DOMAIN_MISMATCH", "rotation context label is invalid")
        if fields[2] == fields[3]:
            fail("INVALID_ROTATION", "old and new identity keys are identical")
        if struct.unpack(">Q", fields[4])[0] == 0:
            fail("INVALID_ROTATION", "rotation counter zero is invalid")
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


def canonical_digest(encoded: bytes, expected_kind: int) -> bytes:
    parse_object(encoded, expected_kind)
    return hashlib.sha256(encoded).digest()


def fixture_hex(
    fixture: Mapping[str, Any], section: str, name: str, length: int
) -> bytes:
    values = require_mapping(fixture.get(section), f"fixture.{section}")
    return decode_hex(values.get(name), f"fixture.{section}.{name}", length)


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
            (6, fixture_hex(fixture, "identity", "initiator_key_hex", 32)),
            (7, fixture_hex(fixture, "identity", "responder_key_hex", 32)),
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
            (4, fixture_hex(fixture, "identity", "initiator_key_hex", 32)),
            (5, fixture_hex(fixture, "identity", "responder_key_hex", 32)),
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
            (2, fixture_hex(fixture, "rotation", "old_key_hex", 32)),
            (3, fixture_hex(fixture, "rotation", "new_key_hex", 32)),
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
    if len(public_key) != 32:
        fail(
            "INVALID_LENGTH",
            "device identifier public key must contain 32 octets",
        )
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
        public_key = fixture_hex(
            fixture, "identity", "initiator_key_hex", 32
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
        expected = decode_hex(
            inputs.get("expected_sha256_hex"),
            "input.expected_sha256_hex",
            32,
        )
        encoded = build_device_identifier(public_key)
        actual = hashlib.sha256(encoded).digest()
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


def load_wordlist(path: Path, expected_sha256: str) -> List[str]:
    encoded = path.read_bytes()
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
    expected_wordlist_sha = require_string(
        profile.get("wordlist_sha256"), "profile.wordlist_sha256"
    )
    wordlist = load_wordlist(
        manifest_path.with_name("wordlist.txt"), expected_wordlist_sha
    )

    fixtures = require_mapping(manifest.get("fixtures"), "fixtures")
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
        default=Path(__file__).with_name("vectors.json"),
    )
    args = parser.parse_args()
    try:
        with args.manifest.open("r", encoding="utf-8") as source:
            manifest = json.load(source)
        return validate_manifest(require_mapping(manifest, "manifest"), args.manifest)
    except (OSError, json.JSONDecodeError, VectorError) as error:
        print(f"security vector validation failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
