# XnnTransfer v1 golden vectors

`vectors.json` is a byte-exact, structured manifest for the normative framing
rules in `protocol/spec/v1.md`.

## Format

- `format_version` versions this fixture format, not the wire protocol.
- `frames` maps a stable name to a direction and complete hexadecimal frame.
- `cases` lists frame names in receive order to form one transcript.
- `expect.result` is `accept` or `reject`.
- A rejected case names the normative machine error in `expect.error`.
- Message IDs are checked independently for each direction.

The manifest contains canonical legal examples and malformed examples for
truncation, oversized declarations, unknown types and fields, reserved bits,
stream scope, TLV ordering and duplication, role/state order, sequence gaps,
version downgrade, capability selection, negotiated limits, ACK mismatch, and
PING/PONG correlation.

Run the self-contained standard-library validator from the repository root:

```bash
python3 protocol/testdata/v1/validate_vectors.py
```

The validator is a fixture oracle for framing and negotiation. It is not a
production parser, transport-security implementation, or replacement for the
native parser tests owned by XT-006.
