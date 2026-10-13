# KaOS verification and testing

Verification combines deterministic C checks, real WAMR/UDP process tests,
finite TLA+ models and physical ESP32 measurements. Each supports a different
claim; host success and cross-compilation alone do not establish target capacity.

## Run the checks

After the [host build](usage.md#build-the-host), run from the repository root:

```sh
ctest --test-dir /tmp/kaos-guide-host --output-on-failure
```

CTest includes C suites, WAMR tests, graph-operation Python process tests and sibling
Python regression discovery when `CORE_BUILD_HOST` and the historical P2 fixture option are enabled. Process
tests launch their own devices and need loopback UDP, subprocesses and signals.
The sibling Python environment must be available to `uv run --offline`.

For deterministic core checks without WAMR or Python:

```sh
cmake -S verification/graph_core -B /tmp/kaos-guide-core -G Ninja \
  -DCORE_SANITIZE=ON
cmake --build /tmp/kaos-guide-core -j4
ctest --test-dir /tmp/kaos-guide-core --output-on-failure
```

Run the separate larger-profile suite for eight-channel scopes and capacity:

```sh
cmake -S verification/graph_core/four -B /tmp/kaos-guide-four -G Ninja \
  -DCORE_SANITIZE=ON
cmake --build /tmp/kaos-guide-four -j4
ctest --test-dir /tmp/kaos-guide-four --output-on-failure
```

This fixture selects `CORE_FOUR_NODE_PROFILE` and currently invokes
`/opt/wasi-sdk/bin/clang` directly. Its capacity tests construct requests in C.
It does not extend the Python encoder's smaller request limits.

Sanitizer builds use ASan, fail-fast UBSan and leak checking. WAMR tests disable
ASan fake stacks (`detect_stack_use_after_return=0`) because the pinned engine
checks native stack addresses. Stack-use-after-return coverage is therefore
excluded for those tests. Do not hide sanitizer startup failures as passing runs.

## Find the right test

Sources are under [verification/graph_core](../../verification/graph_core/).

| Change or claim | Relevant checks |
|---|---|
| Shared loop, artifact integration and adapter | `host`, `host_scheduling`, `host_control`, `host_udp`; P4–P7 process scenarios use `kaos_host` |
| Parsing, IDs and capacity | `request`, `admission`, `dynamic_binding`, `channel_admission`, `p6_admission` |
| Evidence and wire transport | `report`, `object_facts`, `coap`, `coap_server`, `artifact`, `control` |
| Startup, exit, cancellation and memory | `node`, `wamr`, `wamr_entry`, `wamr_budget`, `wamr_capacity` |
| Permissions, queues and edge cleanup | `endpoint`, `edge`, `wamr_channel`, `p4_process` |
| Predicate startup and optional channels | `p5`, `p5_process`, `p5_capacity` |
| Full removal and overlapping observers | `p6`, `p6_control`, `p6_process`, `p6_capacity` |
| ACKs, retirement and collector | `p7`, `p7_control`, `p7_lifecycle`, `p7_process`, `p7_capacity`; sibling collector tests |

CTest names have a `graph_` prefix. Use `ctest --test-dir <build> -N` to inspect
the configured tests and `-R '<expression>'` for a focused run. Capacity entries
belong to the separate four-node build.

For lifecycle changes, exercise both race orderings, expiry at
equality, late callbacks, partial allocation, exact cleanup scope and unchanged
unrelated objects. Distinguish actual owner state from accepted evidence.
Check resource limits just below, at and above each bound. Required behavior
includes no execution before authorization, receiver activation before sender
permission, no deadline renewal, and no CLEANED before full resource release.

## Formal checks

| Model | Scope |
|---|---|
| [Lifetime](../../verification/graph_core/model/Lifetime.tla) | Access tickets, actions, closure and reclamation primitives |
| [Admission](../../verification/graph_core/model/Admission.tla) | Finite reservations, identity, staged work and release |
| [NodeRuntime](../../verification/graph_core/model/NodeRuntime.md) | Two fixed no-edge identities, worker mailboxes, startup/exit/deadline races and disposal; no slot reuse |
| [RuntimeWakeup](../../verification/graph_core/model/RuntimeWakeup.md) | Two ESP32 semaphore/mailbox slots, retained wakeups and conditional progress |

Example TLC invocation, with Java and the existing Toolbox JAR:

```sh
java -XX:+UseParallelGC -Xmx1g -cp /opt/TLA+Toolbox/tla2tools.jar tlc2.TLC \
  -workers 1 -seed 42 -metadir /tmp/kaos-guide-wakeup-tlc \
  -config verification/graph_core/model/RuntimeWakeup.cfg \
  verification/graph_core/model/RuntimeWakeup.tla
```

A positive check requires its configured properties to hold. Negative and
witness checks require their specified counterexample and trace, so a generic
nonzero exit is insufficient. Liveness depends on each model's scheduling,
clock, delivery and termination assumptions. Finite checks do not prove C/WAMR
memory safety or physical timing. The final composition covering current
queues, workers, timers and lifetimes remains open.

## Physical ESP32 fixture

With the pinned runtime/toolchain and ESP-IDF 5.5 environment loaded:

```sh
idf.py -C verification/graph_core/espidf_p7 -B /tmp/kaos-guide-esp32 build
idf.py -C verification/graph_core/espidf_p7 -B /tmp/kaos-guide-esp32 \
  -p /dev/ttyUSB0 flash monitor
```

The second command replaces the board firmware. The fixture targets original
ESP32 without PSRAM, stages artifacts locally and exercises full queues,
overlapping removals, ACKs and retirement. It does not instantiate Wi-Fi, the
UDP control adapter or a networked collector. Its final success marker is
`P7 capacity PASS: live ACKs, later evidence, pressure eviction and full reclamation`.

## Recorded evidence and remaining gates

The [P7 verification record](../../verification/graph_core/p7_verification.json)
records the following results on 12 October 2026:

| Check | Recorded result |
|---|---|
| Default host build | 32 CTest entries passed |
| Python discovery within that run | 96 discovered: 76 passed, 20 legacy integration cases skipped |
| Four-node host build | 11 CTest entries passed |
| Physical ESP32 | Three lifecycles; four WAMR nodes, 16 bindings, 32 full queues and 24 operation slots |
| Full-queue heap | 40,340 bytes free; largest block 18,432 bytes |
| After each lifecycle | 153,268 bytes free; zero final reservations and records |

These are retained run results, not a claim that a fresh checkout has passed.
The physical fixture supports its finite local workload; three repeated cycles
do not establish long-term fragmentation, arbitrary guest budgets or Wi-Fi
capacity. Heap measurements exclude static pools and must be read alongside
native stacks, runtime arenas and platform costs. Total free heap does not
guarantee a sufficiently large contiguous allocation.

Still open: distributed Wi-Fi/collector target measurements, representative
application budgets and timing, final composed formal verification, and
ventilation Quality of Control metrics, policies and parameters. Replay after
discarded rejection history can recreate objects; tests characterize that
limitation separately from conformance checks.

For a new qualification run, retain source/configuration/artifact hashes, profile,
commands, logs, injected faults, observed outcomes and outstanding obligations.
Report pass, fail, skipped, blocked, inconclusive and known limitation separately.
Process teardown or missing telemetry cannot supply CLEANED evidence.


## Shared host consolidation

The shared-loop checks exercise admission/duplicate handling, a bounded artifact
restart, rejection of a queued valid artifact at deadline equality, full staging
reclamation and the same scheduler through `step` and `run`. Management tests
cover late closure after cutoff, stale slot/incarnation/token rejection and
non-wrapping request identities. The socket adapter checks retained wakeups,
enrollment, oversized datagrams, send/receive and shutdown. Existing scenario
assertions are preserved; the P2 private-function test changes only its include
path to the verification fixture.

The networked ESP-IDF host builds from `graph_core/ports/espidf`; this adds build
coverage for the shared loop and socket/wakeup adapter. It is not a physical
network qualification or a replacement for the earlier P7 fixture results.
Fresh commands, hashes and outcomes are recorded in
[host_refactor_verification.json](../../verification/graph_core/host_refactor_verification.json).

The subsequent scheduling refactor keeps the existing tasks and gives the host
explicit lifecycle checkpoints. `host_scheduling` uses real WAMR workers with
controlled clocks and an entry barrier. It checks that worker entry does not
advance unrelated operations, that its own strict deadline still denies entry,
that local channel setup reaches dispatch without input, and that a queued
entry loses to an admitted edge removal. It also checks that node expiry wins
over pending activation with a longer channel allowance. Existing test sources remain unchanged.
Fresh results are recorded separately in
[scheduling_refactor_verification.json](../../verification/graph_core/scheduling_refactor_verification.json).
