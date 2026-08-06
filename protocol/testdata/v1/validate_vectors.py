#!/usr/bin/env python3
"""Validate the byte-exact XnnTransfer v1 golden vectors."""

from __future__ import annotations

import argparse
import json
import struct
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any


MAGIC = b"XNNT"
FIXED_HEADER_LENGTH = 28
MAX_HEADER_LENGTH = 256
MAX_BODY_LENGTH = 1_048_576
MAX_FIELDS = 256

INITIATOR_TO_RESPONDER = "initiator_to_responder"
RESPONDER_TO_INITIATOR = "responder_to_initiator"
DIRECTIONS = {INITIATOR_TO_RESPONDER, RESPONDER_TO_INITIATOR}

HELLO = 0x0001
NEGOTIATE = 0x0002
NEGOTIATE_ACK = 0x0003
ERROR = 0x0004
PING = 0x0005
PONG = 0x0006
GOAWAY = 0x0007
TRANSPORT_FINISHED = 0x0008

CONNECTION_TYPES = {
    HELLO,
    NEGOTIATE,
    NEGOTIATE_ACK,
    PING,
    PONG,
    GOAWAY,
    TRANSPORT_FINISHED,
}
TRANSFER_TYPES = {
    0x0100,
    0x0101,
    0x0102,
    0x0103,
    0x0104,
    0x0110,
    0x0111,
    0x0112,
    0x0113,
    0x0114,
    0x0120,
    0x0121,
    0x0130,
    0x0131,
    0x0140,
    0x0141,
    0x0142,
}
KNOWN_TYPES = CONNECTION_TYPES | TRANSFER_TYPES | {ERROR}

U8 = 1
U16 = 2
U32 = 3
U64 = 4
BYTES = 5
UTF8 = 6
BOOL = 7
WIRE_LENGTHS = {U8: 1, U16: 2, U32: 4, U64: 8, BOOL: 1}

BASE_TRANSFER_V1 = 0x00010001


class VectorError(Exception):
    """A stable protocol error expected by a malformed vector."""

    def __init__(self, code: str, detail: str) -> None:
        super().__init__(f"{code}: {detail}")
        self.code = code
        self.detail = detail


@dataclass(frozen=True)
class FieldRule:
    wire_type: int
    required: bool = True
    repeated: bool = False


@dataclass(frozen=True)
class Frame:
    major: int
    minor: int
    message_type: int
    stream_id: int
    message_id: int
    fields: dict[int, list[Any]]


HELLO_SCHEMA = {
    1: FieldRule(U16),
    2: FieldRule(U16),
    3: FieldRule(U16),
    4: FieldRule(U16),
    5: FieldRule(U8),
    6: FieldRule(U32, repeated=True),
    7: FieldRule(U32, repeated=True),
    8: FieldRule(U32),
    9: FieldRule(U32),
    10: FieldRule(U16),
}

NEGOTIATE_SCHEMA = {
    1: FieldRule(U16),
    2: FieldRule(U16),
    3: FieldRule(U32, repeated=True),
    4: FieldRule(U32),
    5: FieldRule(U32),
    6: FieldRule(U16),
}

CONTROL_SCHEMAS = {
    HELLO: HELLO_SCHEMA,
    NEGOTIATE: NEGOTIATE_SCHEMA,
    NEGOTIATE_ACK: NEGOTIATE_SCHEMA,
    PING: {1: FieldRule(U64)},
    PONG: {1: FieldRule(U64)},
    TRANSPORT_FINISHED: {
        1: FieldRule(U8),
        2: FieldRule(BYTES),
    },
}


def fail(code: str, detail: str) -> None:
    raise VectorError(code, detail)


def decode_integer(value: bytes) -> int:
    return int.from_bytes(value, "big")


def decode_value(wire_type: int, value: bytes) -> Any:
    if wire_type in {U8, U16, U32, U64}:
        return decode_integer(value)
    if wire_type == BOOL:
        if value not in {b"\x00", b"\x01"}:
            fail("MALFORMED_MESSAGE", "Boolean value is not canonical")
        return value == b"\x01"
    if wire_type == UTF8:
        try:
            text = value.decode("utf-8", errors="strict")
        except UnicodeDecodeError:
            fail("MALFORMED_MESSAGE", "invalid UTF-8")
        if "\x00" in text:
            fail("MALFORMED_MESSAGE", "UTF-8 contains U+0000")
        return text
    return value


def parse_tlvs(data: bytes, schema: dict[int, FieldRule]) -> dict[int, list[Any]]:
    offset = 0
    field_count = 0
    previous_id = -1
    fields: dict[int, list[Any]] = {}

    while offset < len(data):
        if len(data) - offset < 8:
            fail("MALFORMED_MESSAGE", "trailing partial TLV header")
        field_id, wire_type, flags, value_length = struct.unpack_from(
            ">HBBI", data, offset
        )
        offset += 8
        field_count += 1
        if field_count > MAX_FIELDS:
            fail("LIMIT_EXCEEDED", "too many TLV fields")
        if field_id < previous_id:
            fail("MALFORMED_MESSAGE", "TLV fields are out of order")
        if wire_type not in {U8, U16, U32, U64, BYTES, UTF8, BOOL}:
            fail("MALFORMED_MESSAGE", "reserved wire type")
        if flags & ~0x01:
            fail("MALFORMED_MESSAGE", "reserved field flag")
        if value_length > len(data) - offset:
            fail("MALFORMED_MESSAGE", "TLV value is truncated")

        value = data[offset : offset + value_length]
        offset += value_length
        rule = schema.get(field_id)
        if rule is None:
            if flags & 0x01:
                fail("UNKNOWN_CRITICAL_FIELD", f"unknown field {field_id}")
            previous_id = field_id
            continue
        if rule.required and not flags & 0x01:
            fail("MALFORMED_MESSAGE", f"required field {field_id} is not critical")
        if wire_type != rule.wire_type:
            fail("MALFORMED_MESSAGE", f"field {field_id} has wrong wire type")
        expected_length = WIRE_LENGTHS.get(wire_type)
        if expected_length is not None and value_length != expected_length:
            fail("MALFORMED_MESSAGE", f"field {field_id} has wrong length")
        if field_id in fields and not rule.repeated:
            fail("MALFORMED_MESSAGE", f"field {field_id} is duplicated")
        fields.setdefault(field_id, []).append(decode_value(wire_type, value))
        previous_id = field_id

    for field_id, rule in schema.items():
        if rule.required and field_id not in fields:
            fail("MALFORMED_MESSAGE", f"required field {field_id} is absent")
    return fields


def parse_frame(encoded: bytes) -> Frame:
    if len(encoded) < FIXED_HEADER_LENGTH:
        fail("MALFORMED_FRAME", "fixed header is truncated")

    (
        magic,
        header_length,
        major,
        minor,
        message_type,
        flags,
        stream_id,
        message_id,
        body_length,
    ) = struct.unpack_from(">4sHBBHHIQI", encoded)

    if magic != MAGIC:
        fail("MALFORMED_FRAME", "invalid magic")
    if header_length < FIXED_HEADER_LENGTH:
        fail("MALFORMED_FRAME", "header length is below the fixed header")
    if header_length > MAX_HEADER_LENGTH:
        fail("FRAME_TOO_LARGE", "header length exceeds the hard limit")
    if body_length > MAX_BODY_LENGTH:
        fail("FRAME_TOO_LARGE", "body length exceeds the hard limit")
    if flags != 0:
        fail("MALFORMED_FRAME", "reserved frame flags are nonzero")
    if message_id == 0:
        fail("MESSAGE_ID_VIOLATION", "message ID zero is reserved")
    if major != 1:
        fail("UNSUPPORTED_VERSION", f"unsupported frame major {major}")
    if message_type not in KNOWN_TYPES:
        fail("UNSUPPORTED_MESSAGE", f"unknown message type {message_type:#06x}")

    total_length = header_length + body_length
    if total_length != len(encoded):
        fail("MALFORMED_FRAME", "encoded length differs from declared length")

    header_extensions = encoded[FIXED_HEADER_LENGTH:header_length]
    parse_tlvs(header_extensions, {})

    if message_type in CONNECTION_TYPES and stream_id != 0:
        fail("STATE_VIOLATION", "connection message has a nonzero stream")
    if message_type in TRANSFER_TYPES and stream_id == 0:
        fail("STATE_VIOLATION", "transfer message has stream zero")

    schema = CONTROL_SCHEMAS.get(message_type, {})
    body = encoded[header_length:total_length]
    fields = parse_tlvs(body, schema)
    return Frame(major, minor, message_type, stream_id, message_id, fields)


def one(fields: dict[int, list[Any]], field_id: int) -> Any:
    return fields[field_id][0]


def validate_sorted_unique(values: list[int], name: str) -> None:
    if values != sorted(set(values)):
        fail("MALFORMED_MESSAGE", f"{name} values are not sorted and unique")


def validate_hello(frame: Frame, direction: str) -> None:
    fields = frame.fields
    minimum = (one(fields, 1), one(fields, 2))
    maximum = (one(fields, 3), one(fields, 4))
    expected_role = 1 if direction == INITIATOR_TO_RESPONDER else 2
    capabilities = fields[6]
    required = fields[7]

    if minimum > maximum:
        fail("MALFORMED_MESSAGE", "version range is reversed")
    if minimum[0] == 0 or any(value > 255 for value in (*minimum, *maximum)):
        fail("MALFORMED_MESSAGE", "version component is outside header range")
    if one(fields, 5) != expected_role:
        fail("STATE_VIOLATION", "HELLO role conflicts with transport role")
    validate_sorted_unique(capabilities, "capability")
    validate_sorted_unique(required, "required capability")
    if len({capability >> 16 for capability in capabilities}) != len(capabilities):
        fail("MALFORMED_MESSAGE", "capability ID advertises multiple versions")
    if BASE_TRANSFER_V1 not in capabilities or BASE_TRANSFER_V1 not in required:
        fail("UNSUPPORTED_CAPABILITY", "BASE_TRANSFER_V1 is not required")
    if not set(required).issubset(capabilities):
        fail("UNSUPPORTED_CAPABILITY", "required capability was not advertised")
    if not 8_192 <= one(fields, 8) <= MAX_BODY_LENGTH:
        fail("LIMIT_EXCEEDED", "receive_max_body is outside v1 limits")
    if not 1_048_576 <= one(fields, 9) <= 67_108_864:
        fail("LIMIT_EXCEEDED", "receive_max_in_flight is outside v1 limits")
    if not 1 <= one(fields, 10) <= 32:
        fail("LIMIT_EXCEEDED", "receive_max_streams is outside v1 limits")


def highest_common_version(
    initiator_fields: dict[int, list[Any]],
    responder_fields: dict[int, list[Any]],
) -> tuple[int, int]:
    initiator_min = (one(initiator_fields, 1), one(initiator_fields, 2))
    initiator_max = (one(initiator_fields, 3), one(initiator_fields, 4))
    responder_min = (one(responder_fields, 1), one(responder_fields, 2))
    responder_max = (one(responder_fields, 3), one(responder_fields, 4))
    minimum = max(initiator_min, responder_min)
    maximum = min(initiator_max, responder_max)
    if minimum > maximum:
        fail("UNSUPPORTED_VERSION", "HELLO ranges do not intersect")
    return maximum


class TranscriptValidator:
    def __init__(self) -> None:
        self.next_message_id = {
            INITIATOR_TO_RESPONDER: 1,
            RESPONDER_TO_INITIATOR: 1,
        }
        self.hellos: dict[str, Frame] = {}
        self.negotiated_fields: dict[int, list[Any]] | None = None
        self.negotiation_complete = False
        self.finished_directions: set[str] = set()
        self.established = False
        self.ping_tokens: dict[str, int] = {}

    def process(self, direction: str, encoded: bytes) -> None:
        if direction not in DIRECTIONS:
            fail("MALFORMED_FRAME", f"invalid vector direction {direction!r}")
        frame = parse_frame(encoded)

        expected_id = self.next_message_id[direction]
        if frame.message_id != expected_id:
            fail(
                "MESSAGE_ID_VIOLATION",
                f"expected message ID {expected_id}, got {frame.message_id}",
            )
        self.next_message_id[direction] += 1

        expected_version = (
            (
                one(self.negotiated_fields, 1),
                one(self.negotiated_fields, 2),
            )
            if self.negotiation_complete and self.negotiated_fields is not None
            else (1, 0)
        )
        if (frame.major, frame.minor) != expected_version:
            fail("UNSUPPORTED_VERSION", "frame header uses the wrong version")

        if frame.message_type == HELLO:
            self.process_hello(direction, frame)
        elif frame.message_type == NEGOTIATE:
            self.process_negotiate(direction, frame)
        elif frame.message_type == NEGOTIATE_ACK:
            self.process_negotiate_ack(direction, frame)
        elif frame.message_type == TRANSPORT_FINISHED:
            self.process_transport_finished(direction, frame)
        elif not self.established:
            fail("STATE_VIOLATION", "message is not allowed before transport binding")
        elif frame.message_type == PING:
            self.process_ping(direction, frame)
        elif frame.message_type == PONG:
            self.process_pong(direction, frame)

    def process_hello(self, direction: str, frame: Frame) -> None:
        if self.established or direction in self.hellos:
            fail("STATE_VIOLATION", "HELLO is repeated or late")
        validate_hello(frame, direction)
        self.hellos[direction] = frame

    def expected_negotiation(self) -> dict[int, list[Any]]:
        if set(self.hellos) != DIRECTIONS:
            fail("STATE_VIOLATION", "NEGOTIATE arrived before both HELLOs")
        initiator = self.hellos[INITIATOR_TO_RESPONDER].fields
        responder = self.hellos[RESPONDER_TO_INITIATOR].fields
        version = highest_common_version(initiator, responder)
        capabilities = sorted(set(initiator[6]) & set(responder[6]))
        required = set(initiator[7]) | set(responder[7])
        if not required.issubset(capabilities):
            fail("UNSUPPORTED_CAPABILITY", "a required capability is unavailable")
        return {
            1: [version[0]],
            2: [version[1]],
            3: capabilities,
            4: [min(one(initiator, 8), one(responder, 8))],
            5: [min(one(initiator, 9), one(responder, 9))],
            6: [min(one(initiator, 10), one(responder, 10))],
        }

    def process_negotiate(self, direction: str, frame: Frame) -> None:
        if direction != INITIATOR_TO_RESPONDER:
            fail("STATE_VIOLATION", "responder sent NEGOTIATE")
        if self.negotiated_fields is not None:
            fail("STATE_VIOLATION", "NEGOTIATE is repeated")
        expected = self.expected_negotiation()
        selected_version = (one(frame.fields, 1), one(frame.fields, 2))
        expected_version = (one(expected, 1), one(expected, 2))
        if selected_version != expected_version:
            fail("DOWNGRADE_DETECTED", "highest common version was not selected")
        if frame.fields[3] != expected[3]:
            fail("UNSUPPORTED_CAPABILITY", "selected capability set is incorrect")
        if any(frame.fields[field_id] != expected[field_id] for field_id in (4, 5, 6)):
            fail("MALFORMED_MESSAGE", "selected limits are not exact minima")
        self.negotiated_fields = frame.fields

    def process_negotiate_ack(self, direction: str, frame: Frame) -> None:
        if direction != RESPONDER_TO_INITIATOR:
            fail("STATE_VIOLATION", "initiator sent NEGOTIATE_ACK")
        if self.negotiated_fields is None:
            fail("STATE_VIOLATION", "NEGOTIATE_ACK arrived before NEGOTIATE")
        if self.negotiation_complete:
            fail("STATE_VIOLATION", "NEGOTIATE_ACK is repeated")
        if frame.fields != self.negotiated_fields:
            fail("MALFORMED_MESSAGE", "NEGOTIATE_ACK does not match selection")
        self.negotiation_complete = True

    def process_transport_finished(self, direction: str, frame: Frame) -> None:
        if not self.negotiation_complete:
            fail("STATE_VIOLATION", "TRANSPORT_FINISHED arrived before negotiation ACK")
        if direction in self.finished_directions:
            fail("STATE_VIOLATION", "TRANSPORT_FINISHED is repeated")
        expected_role = 1 if direction == INITIATOR_TO_RESPONDER else 2
        if one(frame.fields, 1) != expected_role:
            fail("STATE_VIOLATION", "TRANSPORT_FINISHED role conflicts with direction")
        verify_data = one(frame.fields, 2)
        if not 1 <= len(verify_data) <= 64:
            fail("MALFORMED_MESSAGE", "TRANSPORT_FINISHED value exceeds framing bounds")
        self.finished_directions.add(direction)
        self.established = self.finished_directions == DIRECTIONS

    def process_ping(self, direction: str, frame: Frame) -> None:
        reverse = (
            RESPONDER_TO_INITIATOR
            if direction == INITIATOR_TO_RESPONDER
            else INITIATOR_TO_RESPONDER
        )
        self.ping_tokens[reverse] = one(frame.fields, 1)

    def process_pong(self, direction: str, frame: Frame) -> None:
        if self.ping_tokens.pop(direction, None) != one(frame.fields, 1):
            fail("STATE_VIOLATION", "PONG does not match an outstanding PING")


def load_hex(frame_object: dict[str, Any]) -> bytes:
    value = frame_object.get("hex")
    if not isinstance(value, str):
        fail("MALFORMED_FRAME", "vector frame hex is not a string")
    try:
        return bytes.fromhex(value)
    except ValueError:
        fail("MALFORMED_FRAME", "vector frame contains invalid hex")


def validate_case(
    case: dict[str, Any], frame_catalog: dict[str, dict[str, Any]]
) -> tuple[bool, str]:
    expected = case["expect"]
    validator = TranscriptValidator()
    actual_error: VectorError | None = None

    try:
        for frame_name in case["frames"]:
            if not isinstance(frame_name, str) or frame_name not in frame_catalog:
                fail("MALFORMED_FRAME", f"unknown vector frame {frame_name!r}")
            frame_object = frame_catalog[frame_name]
            validator.process(frame_object["direction"], load_hex(frame_object))
    except VectorError as error:
        actual_error = error

    if expected["result"] == "accept":
        if actual_error is not None:
            return False, f"unexpected {actual_error}"
        return True, "accepted"

    expected_error = expected["error"]
    if actual_error is None:
        return False, f"expected {expected_error}, but transcript was accepted"
    if actual_error.code != expected_error:
        return (
            False,
            f"expected {expected_error}, got {actual_error.code}: {actual_error.detail}",
        )
    return True, actual_error.code


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "manifest",
        nargs="?",
        type=Path,
        default=Path(__file__).with_name("vectors.json"),
    )
    args = parser.parse_args()

    with args.manifest.open("r", encoding="utf-8") as source:
        manifest = json.load(source)
    if manifest.get("format_version") != 1:
        print("unsupported vector manifest format", file=sys.stderr)
        return 2

    failures = 0
    cases = manifest.get("cases", [])
    frame_catalog = manifest.get("frames", {})
    if not isinstance(frame_catalog, dict):
        print("vector frame catalog is not an object", file=sys.stderr)
        return 2
    names: set[str] = set()
    for case in cases:
        name = case.get("name")
        if not isinstance(name, str) or not name or name in names:
            print(f"[FAIL] invalid or duplicate case name: {name!r}")
            failures += 1
            continue
        names.add(name)
        passed, result = validate_case(case, frame_catalog)
        print(f"[{'PASS' if passed else 'FAIL'}] {name}: {result}")
        failures += not passed

    if failures:
        print(f"{failures} of {len(cases)} vector cases failed.", file=sys.stderr)
        return 1
    print(f"Validated {len(cases)} XnnTransfer v1 vector cases.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
