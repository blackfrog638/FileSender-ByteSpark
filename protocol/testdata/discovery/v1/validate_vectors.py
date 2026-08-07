#!/usr/bin/env python3
"""Validate byte-exact XnnTransfer discovery v1 vectors and cache scenarios."""

from __future__ import annotations

import argparse
import collections
import ipaddress
import json
import struct
import sys
import unicodedata
from dataclasses import dataclass
from pathlib import Path
from typing import Any


HEADER = struct.Struct("!4sBBBBHHQ16sHHHH")
MAGIC = b"XNND"
VERSION = (1, 0)
HEADER_LENGTH = 44
MAX_DATAGRAM = 512
MAX_TLVS = 32
MIN_TTL_SECONDS = 5
MAX_TTL_SECONDS = 60
MAX_CANDIDATES = 256
MAX_SCOPE_CANDIDATES = 64
MAX_ENTRIES = 512
MAX_SCOPE_ENTRIES = 128
MAX_SOURCE_BUCKETS = 1024
MAX_SCOPE_SOURCE_BUCKETS = 128
SOURCE_BUCKET_IDLE_MS = 60_000
TOMBSTONE_MS = 60_000
GROUPS = {4: "239.255.88.78", 6: "ff12::584e:4e44"}
DESTINATION_PORT = 45_878


class VectorError(Exception):
    """A stable vector or protocol failure."""


class Drop(Exception):
    """A stable local discovery drop reason."""

    def __init__(self, code: str) -> None:
        super().__init__(code)
        self.code = code


def require_dict(value: Any, name: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise VectorError(f"{name} must be an object")
    return value


def require_list(value: Any, name: str) -> list[Any]:
    if not isinstance(value, list):
        raise VectorError(f"{name} must be an array")
    return value


def require_int(value: Any, name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise VectorError(f"{name} must be an integer")
    return value


def require_str(value: Any, name: str) -> str:
    if not isinstance(value, str):
        raise VectorError(f"{name} must be a string")
    return value


def decode_hex(value: Any, name: str) -> bytes:
    encoded = require_str(value, name)
    try:
        return bytes.fromhex(encoded)
    except ValueError as error:
        raise VectorError(f"{name} is not hexadecimal") from error


def decode_datagram_fixture(value: Any, name: str) -> bytes:
    if isinstance(value, str):
        return decode_hex(value, name)
    fixture = require_dict(value, name)
    allowed = {"prefix_hex", "repeat_hex", "repeat_count", "suffix_hex"}
    unknown = sorted(set(fixture) - allowed)
    if unknown:
        raise VectorError(f"{name} has unknown keys: {', '.join(unknown)}")
    prefix = decode_hex(fixture.get("prefix_hex", ""), f"{name}.prefix_hex")
    repeated = decode_hex(fixture.get("repeat_hex", ""), f"{name}.repeat_hex")
    repeat_count = require_int(
        fixture.get("repeat_count", 0),
        f"{name}.repeat_count",
    )
    suffix = decode_hex(fixture.get("suffix_hex", ""), f"{name}.suffix_hex")
    if repeat_count < 0 or (repeat_count and not repeated):
        raise VectorError(f"{name} has an invalid repeat section")
    return prefix + repeated * repeat_count + suffix


def is_noncharacter(codepoint: int) -> bool:
    return 0xFDD0 <= codepoint <= 0xFDEF or (codepoint & 0xFFFE) == 0xFFFE


def decode_label(value: bytes) -> str:
    if not 1 <= len(value) <= 96:
        raise Drop("DROP_INVALID_LABEL")
    try:
        label = value.decode("utf-8", errors="strict")
    except UnicodeDecodeError as error:
        raise Drop("DROP_INVALID_LABEL") from error
    if len(label) > 64 or unicodedata.normalize("NFC", label) != label:
        raise Drop("DROP_INVALID_LABEL")
    if label[0].isspace() or label[-1].isspace():
        raise Drop("DROP_INVALID_LABEL")
    for scalar in label:
        codepoint = ord(scalar)
        category = unicodedata.category(scalar)
        if (
            codepoint == 0
            or is_noncharacter(codepoint)
            or category.startswith("C")
            or category in {"Zl", "Zp"}
        ):
            raise Drop("DROP_INVALID_LABEL")
    return label


@dataclass(frozen=True)
class Packet:
    message_type: str
    sequence: int
    token: bytes
    service_port: int
    ttl_seconds: int
    label: str | None
    raw: bytes


def parse_datagram(payload: bytes) -> Packet:
    if len(payload) < HEADER_LENGTH:
        raise Drop("DROP_TOO_SHORT")
    if len(payload) > MAX_DATAGRAM:
        raise Drop("DROP_TOO_LARGE")

    (
        magic,
        major,
        minor,
        message_type,
        flags,
        total_length,
        header_length,
        sequence,
        token,
        service_port,
        ttl_seconds,
        tlv_length,
        reserved,
    ) = HEADER.unpack_from(payload)

    if magic != MAGIC:
        raise Drop("DROP_BAD_MAGIC")
    if (major, minor) != VERSION:
        raise Drop("DROP_UNSUPPORTED_VERSION")
    if total_length != len(payload):
        raise Drop("DROP_LENGTH_MISMATCH")
    if header_length != HEADER_LENGTH:
        raise Drop("DROP_BAD_HEADER_LENGTH")
    if flags != 0:
        raise Drop("DROP_NONZERO_FLAGS")
    if reserved != 0:
        raise Drop("DROP_NONZERO_RESERVED")
    if tlv_length != total_length - header_length:
        raise Drop("DROP_LENGTH_MISMATCH")
    if message_type not in {1, 2}:
        raise Drop("DROP_UNKNOWN_MESSAGE")
    if sequence == 0:
        raise Drop("DROP_ZERO_SEQUENCE")
    if token == bytes(16):
        raise Drop("DROP_ZERO_TOKEN")

    if message_type == 1:
        if service_port == 0 or not MIN_TTL_SECONDS <= ttl_seconds <= MAX_TTL_SECONDS:
            raise Drop("DROP_INVALID_ANNOUNCE")
    elif service_port != 0 or ttl_seconds != 0 or tlv_length != 0:
        raise Drop("DROP_INVALID_WITHDRAW")

    offset = header_length
    end = total_length
    previous_id = 0
    tlv_count = 0
    label: str | None = None
    while offset < end:
        if end - offset < 4:
            raise Drop("DROP_TLV_TRUNCATED")
        wire_type, value_length = struct.unpack_from("!HH", payload, offset)
        offset += 4
        if value_length > end - offset:
            raise Drop("DROP_TLV_TRUNCATED")
        field_id = wire_type & 0x7FFF
        critical = (wire_type & 0x8000) != 0
        if field_id == 0:
            raise Drop("DROP_RESERVED_TLV")
        if field_id < previous_id:
            raise Drop("DROP_TLV_ORDER")
        if field_id == previous_id:
            raise Drop("DROP_DUPLICATE_TLV")
        previous_id = field_id
        tlv_count += 1
        if tlv_count > MAX_TLVS:
            raise Drop("DROP_TOO_MANY_TLVS")

        value = payload[offset : offset + value_length]
        offset += value_length
        if field_id == 1:
            if critical:
                raise Drop("DROP_UNKNOWN_CRITICAL_TLV")
            label = decode_label(value)
        elif critical:
            raise Drop("DROP_UNKNOWN_CRITICAL_TLV")

    if offset != end:
        raise Drop("DROP_TLV_TRUNCATED")

    return Packet(
        message_type="ANNOUNCE" if message_type == 1 else "WITHDRAW",
        sequence=sequence,
        token=token,
        service_port=service_port,
        ttl_seconds=ttl_seconds,
        label=label,
        raw=payload,
    )


def validate_metadata(
    source: str,
    destination: str,
    destination_port: int,
) -> ipaddress.IPv4Address | ipaddress.IPv6Address:
    try:
        source_ip = ipaddress.ip_address(source)
        destination_ip = ipaddress.ip_address(destination)
    except ValueError as error:
        raise Drop("DROP_INVALID_SOURCE") from error
    if (
        source_ip.is_unspecified
        or source_ip.is_multicast
        or source_ip.is_loopback
        or (source_ip.version == 4 and source_ip.packed[0] in {0, 255})
        or (source_ip.version == 6 and source_ip.ipv4_mapped is not None)
    ):
        raise Drop("DROP_INVALID_SOURCE")
    if (
        source_ip.version != destination_ip.version
        or destination_ip != ipaddress.ip_address(GROUPS[source_ip.version])
        or destination_port != DESTINATION_PORT
    ):
        raise Drop("DROP_WRONG_DESTINATION")
    return source_ip


@dataclass
class TokenBucket:
    rate: float
    burst: float
    tokens: float
    updated_ms: int

    @classmethod
    def full(cls, rate: float, burst: float, now_ms: int) -> "TokenBucket":
        return cls(rate=rate, burst=burst, tokens=burst, updated_ms=now_ms)

    def available(self, now_ms: int) -> float:
        elapsed = max(0, now_ms - self.updated_ms) / 1000.0
        return min(self.burst, self.tokens + elapsed * self.rate)

    def consume(self, now_ms: int) -> None:
        self.tokens = self.available(now_ms) - 1.0
        self.updated_ms = now_ms


@dataclass
class Entry:
    kind: str
    interface: str
    family: int
    source: str
    token: bytes
    highest: int
    raw: bytes
    service_port: int = 0
    label: str | None = None
    deadline_ms: int = 0
    retain_until_ms: int = 0

    @property
    def key(self) -> tuple[str, int, str, bytes]:
        return (self.interface, self.family, self.source, self.token)


class DiscoveryModel:
    def __init__(self) -> None:
        self.now_ms = 0
        self.entries: dict[tuple[str, int, str, bytes], Entry] = {}
        self.local_tokens: set[tuple[str, int, bytes]] = set()
        self.global_bucket = TokenBucket.full(256, 512, 0)
        self.interface_buckets: dict[str, TokenBucket] = {}
        self.source_buckets: dict[tuple[str, str], TokenBucket] = {}

    def _candidate_count(self, interface: str | None = None) -> int:
        return sum(
            entry.kind == "candidate"
            and (interface is None or entry.interface == interface)
            for entry in self.entries.values()
        )

    def _entry_count(self, interface: str | None = None) -> int:
        return sum(
            interface is None or entry.interface == interface
            for entry in self.entries.values()
        )

    def counts(self) -> dict[str, int]:
        candidates = self._candidate_count()
        return {
            "candidates": candidates,
            "tombstones": len(self.entries) - candidates,
        }

    def advance(self, now_ms: int) -> list[dict[str, Any]]:
        if now_ms < self.now_ms:
            raise VectorError("scenario monotonic time moved backwards")
        self.now_ms = now_ms
        events: list[dict[str, Any]] = []
        for key in sorted(self.entries, key=repr):
            entry = self.entries.get(key)
            if entry is None:
                continue
            if entry.kind == "candidate" and now_ms >= entry.deadline_ms:
                entry.kind = "tombstone"
                entry.raw = b""
                entry.service_port = 0
                entry.label = None
                entry.retain_until_ms = now_ms + TOMBSTONE_MS
                events.append(self._event("expired", entry, "ttl"))
            elif entry.kind == "tombstone" and now_ms >= entry.retain_until_ms:
                del self.entries[key]
        return events

    def _admit(self, interface: str, source: str) -> bool:
        interface_bucket = self.interface_buckets.setdefault(
            interface,
            TokenBucket.full(128, 256, self.now_ms),
        )
        for key in list(self.source_buckets):
            bucket = self.source_buckets[key]
            if self.now_ms - bucket.updated_ms >= SOURCE_BUCKET_IDLE_MS:
                del self.source_buckets[key]
        source_key = (interface, source)
        source_bucket = self.source_buckets.get(source_key)
        if source_bucket is None:
            interface_source_count = sum(
                key[0] == interface for key in self.source_buckets
            )
            if (
                len(self.source_buckets) >= MAX_SOURCE_BUCKETS
                or interface_source_count >= MAX_SCOPE_SOURCE_BUCKETS
            ):
                return False
            source_bucket = TokenBucket.full(8, 16, self.now_ms)
            self.source_buckets[source_key] = source_bucket
        buckets = (self.global_bucket, interface_bucket, source_bucket)
        if any(bucket.available(self.now_ms) < 1.0 for bucket in buckets):
            return False
        for bucket in buckets:
            bucket.consume(self.now_ms)
        return True

    def _event(
        self,
        event: str,
        entry: Entry,
        reason: str | None = None,
    ) -> dict[str, Any]:
        result: dict[str, Any] = {
            "event": event,
            "interface": entry.interface,
            "source": entry.source,
            "token": entry.token.hex(),
        }
        if reason is not None:
            result["reason"] = reason
        return result

    def _capacity_available(self, interface: str) -> bool:
        return (
            self._candidate_count() < MAX_CANDIDATES
            and self._candidate_count(interface) < MAX_SCOPE_CANDIDATES
            and self._entry_count() < MAX_ENTRIES
            and self._entry_count(interface) < MAX_SCOPE_ENTRIES
        )

    def _candidate_capacity_available(self, interface: str) -> bool:
        return (
            self._candidate_count() < MAX_CANDIDATES
            and self._candidate_count(interface) < MAX_SCOPE_CANDIDATES
        )

    def _entry_capacity_available(self, interface: str) -> bool:
        return (
            self._entry_count() < MAX_ENTRIES
            and self._entry_count(interface) < MAX_SCOPE_ENTRIES
        )

    def receive(
        self,
        *,
        at_ms: int,
        interface: str,
        source: str,
        destination: str,
        destination_port: int,
        payload: bytes,
        truncated: bool = False,
    ) -> tuple[str, list[dict[str, Any]]]:
        events = self.advance(at_ms)
        try:
            source_ip = validate_metadata(source, destination, destination_port)
            if not self._admit(interface, str(source_ip)):
                raise Drop("DROP_RATE_LIMIT")
            if truncated:
                raise Drop("DROP_TRUNCATED")
            packet = parse_datagram(payload)
        except Drop as drop:
            return drop.code, events

        family = source_ip.version
        if (interface, family, packet.token) in self.local_tokens:
            return "DROP_SELF", events

        key = (interface, family, str(source_ip), packet.token)
        entry = self.entries.get(key)
        if packet.message_type == "WITHDRAW":
            if entry is None:
                if not self._entry_capacity_available(interface):
                    return "DROP_CAPACITY", events
                self.entries[key] = Entry(
                    kind="tombstone",
                    interface=interface,
                    family=family,
                    source=str(source_ip),
                    token=packet.token,
                    highest=packet.sequence,
                    raw=b"",
                    retain_until_ms=self.now_ms + TOMBSTONE_MS,
                )
                return "TOMBSTONED", events
            if packet.sequence <= entry.highest:
                return "DROP_STALE", events
            if entry.kind == "candidate":
                events.append(self._event("expired", entry, "withdrawn"))
            entry.kind = "tombstone"
            entry.highest = packet.sequence
            entry.raw = b""
            entry.service_port = 0
            entry.label = None
            entry.deadline_ms = 0
            entry.retain_until_ms = self.now_ms + TOMBSTONE_MS
            return "WITHDRAWN", events

        if entry is None:
            if not self._capacity_available(interface):
                return "DROP_CAPACITY", events
            entry = Entry(
                kind="candidate",
                interface=interface,
                family=family,
                source=str(source_ip),
                token=packet.token,
                highest=packet.sequence,
                raw=packet.raw,
                service_port=packet.service_port,
                label=packet.label,
                deadline_ms=self.now_ms + packet.ttl_seconds * 1000,
            )
            self.entries[key] = entry
            events.append(self._event("appeared", entry))
            return "APPEARED", events

        if entry.kind == "tombstone" and packet.sequence <= entry.highest:
            return "DROP_STALE", events
        if packet.sequence < entry.highest:
            return "DROP_STALE", events
        if packet.sequence == entry.highest:
            if packet.raw == entry.raw:
                return "DROP_DUPLICATE", events
            return "DROP_SEQUENCE_CONFLICT", events
        if entry.kind == "tombstone" and not self._candidate_capacity_available(
            interface
        ):
            return "DROP_CAPACITY", events

        visible_changed = (
            entry.kind == "tombstone"
            or entry.service_port != packet.service_port
            or entry.label != packet.label
        )
        was_tombstone = entry.kind == "tombstone"
        entry.kind = "candidate"
        entry.highest = packet.sequence
        entry.raw = packet.raw
        entry.service_port = packet.service_port
        entry.label = packet.label
        entry.deadline_ms = self.now_ms + packet.ttl_seconds * 1000
        entry.retain_until_ms = 0
        if was_tombstone:
            events.append(self._event("appeared", entry))
            return "APPEARED", events
        if visible_changed:
            events.append(self._event("updated", entry))
            return "UPDATED", events
        return "REFRESHED", events

    def remove_interface(
        self,
        *,
        at_ms: int,
        interface: str,
    ) -> list[dict[str, Any]]:
        events = self.advance(at_ms)
        for key in sorted(self.entries, key=repr):
            entry = self.entries.get(key)
            if entry is None or entry.interface != interface:
                continue
            if entry.kind == "candidate":
                events.append(self._event("expired", entry, "interface_removed"))
            del self.entries[key]
        self.local_tokens = {item for item in self.local_tokens if item[0] != interface}
        self.interface_buckets.pop(interface, None)
        for key in list(self.source_buckets):
            if key[0] == interface:
                del self.source_buckets[key]
        return events

    def wake(self, *, at_ms: int) -> list[dict[str, Any]]:
        if at_ms < self.now_ms:
            raise VectorError("scenario monotonic time moved backwards")
        self.now_ms = at_ms
        events: list[dict[str, Any]] = []
        for key in list(self.entries):
            entry = self.entries[key]
            if entry.kind == "tombstone" and at_ms >= entry.retain_until_ms:
                del self.entries[key]
        for entry in self.entries.values():
            if entry.kind != "candidate":
                continue
            events.append(self._event("expired", entry, "wake"))
            entry.kind = "tombstone"
            entry.raw = b""
            entry.service_port = 0
            entry.label = None
            entry.deadline_ms = 0
            entry.retain_until_ms = self.now_ms + TOMBSTONE_MS
        self.local_tokens.clear()
        return events


def event_projection(events: list[dict[str, Any]]) -> list[str]:
    return [
        event["event"] + (":" + event["reason"] if "reason" in event else "")
        for event in events
    ]


def assert_equal(actual: Any, expected: Any, context: str) -> None:
    if actual != expected:
        raise VectorError(f"{context}: expected {expected!r}, got {actual!r}")


def validate_parse_cases(
    manifest: dict[str, Any],
    datagrams: dict[str, bytes],
) -> int:
    cases = require_list(manifest.get("parse_cases"), "parse_cases")
    seen: set[str] = set()
    for raw_case in cases:
        case = require_dict(raw_case, "parse case")
        case_id = require_str(case.get("id"), "parse case id")
        if case_id in seen:
            raise VectorError(f"duplicate parse case id {case_id!r}")
        seen.add(case_id)
        datagram_name = require_str(case.get("datagram"), f"{case_id}.datagram")
        if datagram_name not in datagrams:
            raise VectorError(f"{case_id}: unknown datagram {datagram_name!r}")
        expected = require_str(case.get("result"), f"{case_id}.result")
        try:
            packet = parse_datagram(datagrams[datagram_name])
            actual = "ACCEPT"
        except Drop as drop:
            packet = None
            actual = drop.code
        assert_equal(actual, expected, case_id)
        if actual == "ACCEPT":
            decoded = require_dict(case.get("decoded"), f"{case_id}.decoded")
            assert packet is not None
            observed = {
                "message_type": packet.message_type,
                "sequence": packet.sequence,
                "token": packet.token.hex(),
                "service_port": packet.service_port,
                "ttl_seconds": packet.ttl_seconds,
                "label": packet.label,
            }
            assert_equal(observed, decoded, f"{case_id}.decoded")
    return len(cases)


def check_action_expectations(
    *,
    case_id: str,
    action_index: int,
    action: dict[str, Any],
    model: DiscoveryModel,
    result: str,
    events: list[dict[str, Any]],
) -> None:
    prefix = f"{case_id}.actions[{action_index}]"
    if "result" in action:
        assert_equal(result, action["result"], f"{prefix}.result")
    if "events" in action:
        assert_equal(
            event_projection(events),
            action["events"],
            f"{prefix}.events",
        )
    if "counts" in action:
        assert_equal(model.counts(), action["counts"], f"{prefix}.counts")
    if "deadline_ms" in action:
        candidates = [
            entry for entry in model.entries.values() if entry.kind == "candidate"
        ]
        if len(candidates) != 1:
            raise VectorError(f"{prefix}: deadline assertion needs one candidate")
        assert_equal(
            candidates[0].deadline_ms,
            action["deadline_ms"],
            f"{prefix}.deadline_ms",
        )


def receive_from_action(
    model: DiscoveryModel,
    action: dict[str, Any],
    datagrams: dict[str, bytes],
    *,
    source: str | None = None,
) -> tuple[str, list[dict[str, Any]]]:
    datagram_name = require_str(action.get("datagram"), "action.datagram")
    if datagram_name not in datagrams:
        raise VectorError(f"unknown scenario datagram {datagram_name!r}")
    actual_source = source or require_str(action.get("source"), "action.source")
    source_version = ipaddress.ip_address(actual_source).version
    truncated = action.get("truncated", False)
    if not isinstance(truncated, bool):
        raise VectorError("action.truncated must be a boolean")
    return model.receive(
        at_ms=require_int(action.get("at_ms"), "action.at_ms"),
        interface=require_str(action.get("interface"), "action.interface"),
        source=actual_source,
        destination=require_str(
            action.get("destination", GROUPS[source_version]),
            "action.destination",
        ),
        destination_port=require_int(
            action.get("destination_port", DESTINATION_PORT),
            "action.destination_port",
        ),
        payload=datagrams[datagram_name],
        truncated=truncated,
    )


def validate_state_cases(
    manifest: dict[str, Any],
    datagrams: dict[str, bytes],
) -> int:
    cases = require_list(manifest.get("state_cases"), "state_cases")
    seen: set[str] = set()
    for raw_case in cases:
        case = require_dict(raw_case, "state case")
        case_id = require_str(case.get("id"), "state case id")
        if case_id in seen:
            raise VectorError(f"duplicate state case id {case_id!r}")
        seen.add(case_id)
        model = DiscoveryModel()
        actions = require_list(case.get("actions"), f"{case_id}.actions")
        for index, raw_action in enumerate(actions):
            action = require_dict(raw_action, f"{case_id}.actions[{index}]")
            operation = require_str(action.get("op"), f"{case_id}.op")
            result = "OK"
            events: list[dict[str, Any]] = []
            if operation == "receive":
                result, events = receive_from_action(model, action, datagrams)
            elif operation == "advance":
                events = model.advance(
                    require_int(action.get("at_ms"), f"{case_id}.at_ms")
                )
            elif operation == "remove_interface":
                events = model.remove_interface(
                    at_ms=require_int(action.get("at_ms"), f"{case_id}.at_ms"),
                    interface=require_str(
                        action.get("interface"),
                        f"{case_id}.interface",
                    ),
                )
            elif operation == "wake":
                events = model.wake(
                    at_ms=require_int(action.get("at_ms"), f"{case_id}.at_ms")
                )
            elif operation == "add_local_token":
                token = decode_hex(action.get("token"), f"{case_id}.token")
                if len(token) != 16:
                    raise VectorError(f"{case_id}: local token must be 16 bytes")
                model.advance(
                    require_int(action.get("at_ms", model.now_ms), "action.at_ms")
                )
                model.local_tokens.add(
                    (
                        require_str(action.get("interface"), "action.interface"),
                        require_int(action.get("family"), "action.family"),
                        token,
                    )
                )
            elif operation == "repeat_receive":
                count = require_int(action.get("count"), f"{case_id}.count")
                observed: collections.Counter[str] = collections.Counter()
                for _ in range(count):
                    repeat_result, repeat_events = receive_from_action(
                        model,
                        action,
                        datagrams,
                    )
                    observed[repeat_result] += 1
                    events.extend(repeat_events)
                expected_results = require_dict(
                    action.get("results"),
                    f"{case_id}.results",
                )
                assert_equal(
                    dict(sorted(observed.items())),
                    expected_results,
                    f"{case_id}.actions[{index}].results",
                )
                result = "REPEATED"
            elif operation == "receive_source_range":
                start = require_int(action.get("start"), f"{case_id}.start")
                count = require_int(action.get("count"), f"{case_id}.count")
                prefix = action.get("prefix")
                network_value = action.get("network")
                if (prefix is None) == (network_value is None):
                    raise VectorError(
                        f"{case_id}: source range needs exactly one "
                        "of prefix or network"
                    )
                network = (
                    ipaddress.ip_network(
                        require_str(network_value, f"{case_id}.network")
                    )
                    if network_value is not None
                    else None
                )
                observed = collections.Counter()
                for value in range(start, start + count):
                    if network is not None:
                        source = str(
                            ipaddress.ip_address(int(network.network_address) + value)
                        )
                        if ipaddress.ip_address(source) not in network:
                            raise VectorError(f"{case_id}: source range leaves network")
                    else:
                        source = require_str(prefix, f"{case_id}.prefix") + f"{value}"
                    range_result, range_events = receive_from_action(
                        model,
                        action,
                        datagrams,
                        source=source,
                    )
                    observed[range_result] += 1
                    events.extend(range_events)
                expected_results = require_dict(
                    action.get("results"),
                    f"{case_id}.results",
                )
                assert_equal(
                    dict(sorted(observed.items())),
                    expected_results,
                    f"{case_id}.actions[{index}].results",
                )
                result = "RANGE_RECEIVED"
            else:
                raise VectorError(f"{case_id}: unknown operation {operation!r}")
            check_action_expectations(
                case_id=case_id,
                action_index=index,
                action=action,
                model=model,
                result=result,
                events=events,
            )
        if "final_counts" in case:
            assert_equal(
                model.counts(),
                case["final_counts"],
                f"{case_id}.final_counts",
            )
    return len(cases)


def load_manifest(path: Path) -> dict[str, Any]:
    try:
        with path.open("r", encoding="utf-8") as source:
            manifest = json.load(source)
    except (OSError, json.JSONDecodeError) as error:
        raise VectorError(f"cannot load {path}: {error}") from error
    result = require_dict(manifest, "manifest")
    if result.get("format_version") != 1:
        raise VectorError("unsupported vector manifest format")
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "manifest",
        nargs="?",
        type=Path,
        default=Path(__file__).with_name("vectors.json"),
    )
    args = parser.parse_args()

    try:
        manifest = load_manifest(args.manifest)
        raw_datagrams = require_dict(manifest.get("datagrams"), "datagrams")
        datagrams = {
            name: decode_datagram_fixture(value, f"datagrams.{name}")
            for name, value in raw_datagrams.items()
        }
        parse_count = validate_parse_cases(manifest, datagrams)
        state_count = validate_state_cases(manifest, datagrams)
    except VectorError as error:
        print(f"discovery vector validation failed: {error}", file=sys.stderr)
        return 1

    print(
        f"Validated {parse_count} discovery datagram cases and "
        f"{state_count} lifecycle cases."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
