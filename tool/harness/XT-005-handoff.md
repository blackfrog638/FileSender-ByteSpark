# Agent handoff

## Delivered

- Task: XT-005 - Package the native library for Flutter desktop
- From owner: packaging-agent
- To owner or reviewer: integration owner
- Branch: `task/XT-005`
- Worktree: `/Users/bytedance/XnnTransfer/XnnTransfer-XT-005`
- Base SHA: `784375866646b9aa3374734ba6f02a00d11b5208`
- Head SHA: `task/XT-005` branch tip containing this handoff
- Worktree clean: Yes, after commit
- Owned paths: `apps/desktop/{linux,macos,windows}/**`,
  `tool/harness/**`, `.github/workflows/**`
- Observable behavior: Debug and Release Flutter desktop builds compile the
  native core for the target configuration and architecture. The library is
  placed next to the Linux and Windows executables and in the macOS app
  `Contents/Frameworks` directory. CI loads each bundled library and requires
  ABI version 1.

## Contracts

- Added or changed: Build and packaging integration only. Added a cross-platform
  bundle smoke command at `tool/harness/desktop_bundle_test.sh`.
- Compatibility impact: None. Existing library names, Dart loader paths, and C
  ABI version 1 are preserved.
- ADR or protocol reference: Not applicable; no public interface or protocol
  changed.

## Verification evidence

- Command: `make verify`
- Result: Passed all repository gates with no skips. Native CMake build and
  CTest passed; Flutter formatting, analysis, and tests passed.
- Command: `make macos-bundle-test`
- Result: Passed Debug and Release. Both app bundles passed code-signature and
  relocatable install-name checks; library architectures matched each app;
  both libraries loaded successfully and returned ABI version 1.
- Command: `find tool/harness -type f -name '*.sh' -exec bash -n {} \;`
- Result: Passed.
- Command: Ruby YAML parse of `.github/workflows/ci.yml`, `git diff --check`,
  and Linux/Windows packaging invariant inspection with `rg`
- Result: Passed.
- Skipped gate and reason: Linux and Windows Flutter bundles cannot be built on
  the macOS worktree host. The new `desktop-packaging` CI matrix performs
  Debug and Release builds and dynamic ABI load checks on native Ubuntu,
  macOS, and Windows runners.

## Residual risk

- Known limitation: Linux and Windows dynamic smoke results remain pending
  until CI runs. CI currently covers the latest hosted OS images, not older
  distribution or Windows runtime baselines.
- Follow-up task: Review the first cross-platform CI run and expand the release
  matrix when minimum supported OS versions are defined.

## Review focus

- Files or invariants requiring close review: Native target inclusion and
  install destinations in Linux/Windows `CMakeLists.txt`; Git Bash path handling
  in the Windows smoke job; macOS architecture/signing behavior for universal
  Release bundles.

## Acceptance

- Accepted by:
- Accepted at:
- Follow-up runtime state: `review`
