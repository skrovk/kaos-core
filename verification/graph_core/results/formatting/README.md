# C formatting verification, 22 October 2026

The [manifest](../../formatting_verification.json) records the formatting policy,
scope, commands, results and limitations. The baseline is the working tree after
the simplification and clock relocation, before this formatting task.

- `source.patch` shows this task's complete C/H changes against that baseline.
- `semantic.patch` compares the mechanically formatted baseline with the final
  code, exposing semantic grouping and guard-clause edits separately.
- `*.review.patch` remove blank lines and indentation to make the control-flow
  changes compact; use the full source to review scopes and locks.
- `baseline.json`, `source-hashes.json` and `preserved-test-hashes.json` retain
  provenance. All 94 captured verification C/H/Python files, including archived
  harnesses, are unchanged.
- `format-check.log` and `token-check.json` record format conformance and the
  token comparison. Mechanical formatting changes no code tokens; thirteen
  functions in five files have the reviewed guard-clause rewrites.
- Build, CTest and runtime process logs retain the actual checks and inner skips.
- `codegen.json` records native `-O2` object comparisons in both profiles.
  Twenty-six of 36 objects are byte-identical; the five files with control-flow
  edits differ in each profile. This is not a proof for the rewritten functions.

The audit scripts retain the paths of this run under
`/tmp/kaos-semantic-format-20261022`. To reproduce elsewhere, adapt those paths,
reconstruct the original `before/graph_core` in a separate directory by reversing
`source.patch` on a copy of the final sources, and check its baseline hashes.
Recreate `formatted/` by applying the root `.clang-format` with version 14.0.4
to that original snapshot. No historical snapshot or test needs to be modified.

Host regression suites pass with ASan/UBSan/LeakSanitizer. The ESP32 result is a
cross-build only; physical timing, stack margin and throughput are not qualified.
