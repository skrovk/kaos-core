# Simplification evidence, 22 October 2026

This record covers steps 0–6 of the
[simplification plan](../../../../implementation_rails/graph_core_simplification_plan.md).
The [verification manifest](../../simplification_verification.json) states scope,
commands, results and limitations. Application-CoAP slice P is not implemented.

| Evidence | Meaning |
|---|---|
| `baseline.json` | SHA-256 of the dirty tree captured before this refactor; Git HEAD is not the baseline. |
| `production.patch`, `changed-source-hashes.json` | Incremental production changes and final changed source/build/test hashes. Deleted UDP paths are represented as deletion/addition. |
| `preserved-test-hashes.json` | All 87 pre-existing C/H/Python verification files remain byte-identical. |
| `default-ctest.log`, `four-ctest.log`, corresponding `*-details.log` | Full 60/60 and 34/34 sanitized suites, including inner Python results/skips and the two new cases. |
| `runtime-process.log` | Nine explicitly enabled runtime process cases, all passing. |
| `protocol-*.log`, `esp-build.log` | Protocol-only build/check and ESP32 network-host cross-build. |
| `equivalence.c`, `equivalence.py`, `differential-*.log` | Generated old/new request/admission and UDP-parser comparisons, in each profile. |
| `sizes.json`, `size_probe.py`, `size-summary.log` | Eight record sizes before/after on native/ESP32 compilers, default/four profiles. |
| `stack.json`, `stack_probe.py`, `stack/`, `esp-compile-commands.json` | Xtensa `-Og` static frames for ten affected translation units before/after in both profiles. |
| `*-configuration.json` | Host CMake profile settings used for the reported runs. |

The probe scripts retain the actual workspace/build paths used for this run.
They are evidence drivers, not new supported build interfaces. To reproduce
them elsewhere, update their `root`, `work` and build paths. Reconstruct
`work/before/graph_core` from a copy of the final source tree and the reverse of
`production.patch`, then verify its hashes against `baseline.json`. Leave the
working source tree untouched. Existing CMake verification targets run the new
characterization cases normally; the generated differential driver additionally
requires the configured host libraries and original sources.

The differential harness compares full registry state at one stable address,
including output pointers, return values and failed-request mutation. UDP cases
also compare `errno` and partial output. Its generated mutations/truncations and
four registry setups are finite coverage, not exhaustive state-space proof. The
new decoded-control case compares both public paths; existing regressions still
supply the behavioral assertions for their shared implementation.

Static frames do not establish the maximum active call chain. For example, the
host's control-handler frame falls from 352 to 128 bytes, but the retained raw
wrapper plus handler totals 416 bytes versus the original 352. UDP decimal
parsing adds 48 bytes; endpoint matching adds a 128-byte helper while the caller
falls from 128 to 64. Host step and receive-batch frames remain 1232 bytes. Full
reports preserve the other changes. Stack configuration and record sizes remain
unchanged; no physical stack margin, timing or throughput claim follows.

The initial sandboxed baseline run could not use sockets/LeakSanitizer normally;
`baseline-tests.log` retains those failures and `baseline-tests-unsandboxed.log`
the successful authorized rerun. Final suites also ran with the required socket
and sanitizer access. WAMR configuration required its generated version header
in the sibling dependency tree. No existing fixture was altered to resolve a
failure. No board execution occurred.
