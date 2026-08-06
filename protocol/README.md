# Protocol workspace

Wire behavior is specified here before implementation. `spec/` contains
versioned normative documents; `testdata/` will contain cross-platform golden
vectors and malformed inputs.

A protocol proposal must define:

- framing and byte order;
- version and capability negotiation;
- authentication and encryption context;
- maximum lengths, counts, and timeouts;
- state transitions and machine-readable errors;
- retry, idempotency, resume, and cancellation semantics;
- compatibility behavior and test vectors.

Discovery is not an authentication mechanism. Do not add transfer metadata or
file contents to unauthenticated discovery packets.
