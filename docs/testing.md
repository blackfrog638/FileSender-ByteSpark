# Test strategy

## Current executable coverage

The harness phase has real checks for:

- C ABI argument validation and version rejection;
- native lifecycle events, callback serialization, and shutdown barriers;
- bounded v1 frame/TLV parsing against legal and hostile vectors;
- generated-fixture drift and parser transcript-state regression;
- byte-exact proposed security-profile vectors for transcript, SAS,
  confirmation, transport binding, and rotation proof inputs;
- protected-identity codecs, repository transitions, per-item CAS, reset and
  stale-generation cleanup, corruption and partial-restore failures, platform
  adapters, and the exact GNOME Keyring profile described below;
- host-independent path and manifest contract vectors for legal limits,
  traversal, collisions, invalid entries, and inconsistent summaries;
- C++ compiler warnings;
- pinned dependency provenance, exact linked versions, Linux-only libsecret
  scoping, a gate-generated Linux runtime link probe, static triplets, and
  absence of dependency dynamic libraries;
- ASan and UBSan over the native test suite;
- short libFuzzer runs over C ABI lifecycle and protocol parser inputs;
- an informational native lifecycle microbenchmark;
- Flutter formatting, static analysis, transfer application-state tests, and
  the real packaged native callback boundary;
- task record, handoff, integration provenance, acceptance, and cleanup
  governance.

This is not file-transfer coverage. Discovery, protected identity storage, and
the TLS provider have focused executable coverage, but authenticated sessions,
manifest/storage enforcement, file I/O, and network recovery are not
implemented yet and therefore are not currently tested.

## Future-task TDD workflow

XT-083 is the first task governed by the schema-v4 TDD workflow. Before claim,
an approved schema-v2 Delivery Plan defines stable criteria, negative
definitions, evidence ownership, required scenarios and assertions, matrix,
topology, and trusted gates. The task record binds those criteria to a
task-type proof mode, deterministic executor where supported, focused gate,
owned Red surface, exact failure fingerprints, and a no-skip policy.

For Red-Green, regression, mutation, and equivalence work:

1. Commit only the declared test, fixture, scenario, golden, snapshot, fuzz, or
   test-registration surface.
2. Run `tool/harness/agent.sh checkpoint XT-NNN red`. The focused gate must pass
   at base and produce the declared Red result without skips, crashes,
   timeouts, missing tools, or unrelated failures.
3. Commit the implementation after the generated checkpoint lifecycle commit.
4. Move to review. The harness replays base, Red, and reviewed head and rejects
   changed oracles, production-before-Red history, stale governance context,
   and hand-authored proof.
5. Acceptance reruns required gates against the exact integrated candidate and
   validates criterion-level jobs, artifacts, binary digests, matrices, and
   topology before publishing the protected integration branch.

Unit, integration, contract snapshot, smoke, E2E, security, reliability, and
performance evidence remain distinct. A generic `make verify` result cannot
replace specialized evidence, and fake/in-memory topology cannot satisfy E2E.

This workflow is future-only. Existing tasks and schema-v1 plans before XT-083
retain their historical validation and are not claimed to have followed TDD.

## Commands

```bash
make verify          # required local completion gate
make dependency-test # pinned vcpkg provenance, versions, and static linkage
make security-test   # ASan, UBSan, and bounded libFuzzer run
make benchmark       # informational native benchmark
make macos-bundle-test # build app, verify signing, and load bundled dylib
make governance-test # isolated task lifecycle and provenance test
make tdd-proof-test  # deterministic Red/checkpoint/review proof
make evidence-test   # exact-SHA criterion evidence validation
```

Use `XNN_TRANSFER_FUZZ_SECONDS=60 make security-test` for a longer local fuzz
run. Crashing inputs must be minimized, committed under the owning fuzz corpus,
and converted to deterministic regression tests.

## Linux protected-store qualification

Linux support is restricted to the current-user GNOME Keyring service at the
root-owned `/usr/bin/gnome-keyring-daemon` executable, the encrypted libsecret
session, and `/org/freedesktop/secrets/collection/login`. The focused CTest
starts that daemon inside isolated D-Bus, home, XDG data, and XDG runtime
directories:

```bash
ctest --test-dir out/build/ci --output-on-failure \
  -R '^xnn_transfer_linux_secret_service_integration_tests$'
```

The test exercises exact attributes and bounds, CRUD and CAS, two-process
contention, daemon restart, locked and denied paths, service-owner replacement,
identity reset and cleanup, missing root and seed, partial restore,
inconsistent rollback, corruption, and deletion. It also verifies that the
payload-free lock is empty and private and that observed payload and seed bytes
do not occur in the persistent keyring files.

CI runs this test both in the normal Linux native/security suites and in the
fixed `ubuntu-24.04` `Linux GNOME Keyring qualification` job. Install
`dbus-x11` and `gnome-keyring` before configuring a local Linux test build.
Passing this gate does not qualify another Secret Service implementation and
does not provide complete-valid-snapshot rollback detection.

## Security test matrix

Each module must add its row before it can claim production readiness.

| Module | Required negative coverage | Current |
| --- | --- | --- |
| C ABI | nulls, short structs, versions, invalid state, lifecycle sequences | Covered foundation |
| Framing | truncation, oversized length, unknown type, downgrade, state order | Covered foundation |
| Pairing | MITM, replay, wrong code, key replacement, revoked peer | Contract vectors covered; implementation blocked |
| Manifest | count/size overflow, duplicate paths, invalid encoding | Contract vectors covered; implementation blocked |
| Storage | traversal, absolute paths, links, collisions, low space, rollback | Blocked |
| Transfer | corrupt/replayed chunks, cancellation, timeout, resume mismatch | Blocked |
| Concurrency | callback-after-free, races, shutdown under load | Partial |

`Blocked` means the production module does not exist. The implementing task
owns both positive and negative tests.

## Performance matrix

Performance measurements run in Release mode and report machine metadata with
results once transfer I/O exists.

| Scenario | Data points | Required metrics | Current |
| --- | --- | --- | --- |
| Native lifecycle | 100,000 create/start/destroy cycles | operations/s | Informational |
| Small files | 10,000 x 1 KiB | files/s, p50/p95 latency, CPU, RSS | Blocked |
| Medium file | 1 GiB | MiB/s, CPU, RSS | Blocked |
| Large file | 20 GiB sparse/generated data | MiB/s, RSS, integrity | Blocked |
| Parallel files | 1/4/16 streams | aggregate MiB/s, fairness, RSS | Blocked |
| Resume | interrupt at 10/50/90% | recovery latency, retransmitted bytes | Blocked |
| Adverse LAN | delay/loss/bandwidth limits | throughput, completion, retries | Blocked |

Do not put unstable absolute performance thresholds on pull requests.
Initially collect scheduled baselines on dedicated machines. Gate regressions
only after variance is known, using a reviewed relative threshold.

## CI policy

- Pull requests: governance contracts, native tests on three platforms, the
  focused GNOME Keyring lifecycle on Ubuntu 24.04, pinned dependency probes,
  Flutter checks, packaging on three platforms, ASan/UBSan, and a short fuzz
  smoke test.
- Scheduled builds: longer fuzz campaigns. Performance trend collection starts
  when transfer I/O benchmarks exist.
- Before release: cross-platform interoperability, low-space, sleep/wake,
  network-change, large-file, and hostile-input suites.

Coverage percentage is a visibility metric, not a security claim. Add
`llvm-cov` reporting when protocol and storage implementations begin; set
thresholds only after meaningful code exists.

Repository CI is a merge gate only when the hosting service protects the
integration/default branches and requires every workflow job. Repository files
cannot enforce that remote setting; the integration owner must audit branch
protection before release work.
