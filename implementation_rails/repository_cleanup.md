# Repository cleanup — 22–23 October 2026

Authorized by the user's request to perform the recommended cleanup, including
retirement of the legacy implementation and its tests. No outstanding graph
feature is made a prerequisite, and no legacy feature-parity claim is made.

## Retired

- Root C sources, legacy `include/`, `port/`, `test/`, root ESP-IDF
  `CMakeLists.txt` and `Kconfig.projbuild`.
- Unused vendored `unicoap/`, including its separate build machinery.
- Tracked legacy test build output and binarydump executable, local Ruff cache
  and graph-verification Python bytecode caches.
- P3/P4 host build options and executable aliases. The four-node build now
  selects `CORE_BUILD_HOST`; the P2 artifact fixture is retained.

The sibling simulator's historical build checks for the retired core and gives
a direct migration message before patching, fetching or compiling. Historical
experiments can still select a pre-retirement checkout. Its legacy Python
scenario tools and tests are retained; the current supported execution paths are
`sim_env.graph_run` and `sim_env.graph_workloads`.

## Preserved

Production graph C/header files, current verification test sources and fixtures,
and all existing evidence. Only verification build configuration changes to use
the canonical host option. The `KAOS_P3_HOST` environment variable remains a
test-harness interface pointing at `kaos_host`; it is not an executable alias.

Historical contracts and evidence remain in place, indexed in [README](README.md),
because their links and original provenance remain useful. No generic `git clean`
or evidence pruning is performed: verification was untracked when cleanup began.
Root README and ignore rules distinguish maintained source from generated caches.

A temporary pre-removal backup is at
`/tmp/kaos-cleanup-20261022/legacy-backup.tar.gz`. It is a local recovery aid,
not a durable archive or a substitute for reviewing and committing this change.

## Validation

Results are recorded in
[`cleanup_verification.json`](../verification/graph_core/cleanup_verification.json).
