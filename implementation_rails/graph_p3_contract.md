# P3 node runtime and no-edge operations

P4 adds the [endpoint, edge and channel ABI](graph_p4_contract.md) and upgrades
the current build to schema 4. P3's operation fields and historical results
below remain unchanged. The P4 qualification also extends the WAMR alignment
correction to unaligned i64/f64 operand cells and guest-memory accesses.

This increment connects schema-3 admission and artifact transfer to real WAMR
execution for **no-edge node addition and removal** (request kinds 1 and 4).
The execution adapter targets Linux x86_64 and the original ESP32 without
PSRAM. The no-edge P3 slice now has host and physical ESP32 qualification;
see the [target results and corrections](#esp32-qualification-and-corrections).
Wi-Fi/channel capacity and the final combined deployment remain P4–P9 work.

The [P2 contract](graph_p2_contract.md) still defines request identity, resource
admission, CoAP, artifact transfer and cumulative evidence. There is no new
wire schema, application-readiness exchange or orchestrator implementation of
device transitions. Other operation kinds retain their P2 admission paths;
this increment does not execute channels or node additions with channels.

## Architecture and ownership

| Code | Responsibility |
| --- | --- |
| [`kaos_node.c`](../graph_core/kaos_node.c) | Serialized no-edge operation decisions: preparation evidence, commit/start authorization, accepted startup, strict deadlines, cleanup and results. `core_node_artifact`, `core_node_step` and `core_node_next_deadline` are its small public interface. |
| [`kaos_runtime.h`](../graph_core/include/kaos_runtime.h) | Runtime boundary: queue `prepare`, then briefly `lock`/`unlock` a fixed per-node mailbox. Commands are start, cancel and dispose; workers publish preparation, INIT, exit and disposal state. |
| [`runtime/kaos_wamr.c`](../graph_core/runtime/kaos_wamr.c) | Two persistent pthread workers, one per node slot; private WAMR handles and allocation arena. Instantiation, guest execution and destruction occur on the worker, outside control transitions. |
| [`runtime/profile.c`](../graph_core/runtime/profile.c) | Reject artifacts outside the restricted inert-instantiation profile before calling WAMR. WAMR then performs full bytecode/type validation. |
| [`ports/p2_host.c`](../graph_core/ports/p2_host.c) | Shared UDP adapter. `CORE_HOST_RUNTIME` builds `kaos_p3_host`, connecting artifact handoff, runtime wakeups, control service and queries. The existing `kaos_p2_host` remains the artifact-only exercise. |

The admission records remain the source of operation/instance identity and
resource ownership. The runtime does not mutate admission, evidence or the
lifetime ledger. `core_node_artifact` takes an already validated, charged
artifact, charges the remaining requested arena and reserves a worker access
ticket. If queueing fails, the caller still owns the artifact and must clean
it up. If queueing succeeds, the worker owns the artifact and arena until
actual disposal. The same worker ticket stays held through preparation,
configuration, main execution and disposal acceptance.

The control task holds a mailbox mutex only across short decisions. Exit and
`main_active` updates share that mutex with INIT acceptance. Cancellation
closes the lifetime and requests termination immediately; disposal is
requested only after all accesses other than the retained worker ticket have
drained. The worker publishes `disposed` after calls/imports have returned
and WAMR handles, artifact and arena are destroyed. Control then accepts the
worker completion, releases its ticket, removes the charges, releases the
node reservation and records STOPPED/CLEANED. A stop request, exit or timeout
alone cannot free capacity or establish CLEANED.

## Guest ABI and restricted artifacts

Required exports use WebAssembly `i32` parameters/results:

```c
int kaos_configure(const unsigned char *configuration, unsigned size);
void kaos_main(void);
```

Configuration is copied into guest memory after startup authorization.
`kaos_configure` must return zero; nonzero return or trap fails startup.
`kaos_main` is called once after successful configuration, with a new check of
cancellation and the original addition deadline before each guest call. The
service-node convention is long-lived main execution. There is no automatic
restart or main re-entry.

Only these optional function imports from module `kaos` are accepted:

| Import | Contract |
| --- | --- |
| `init() -> i32` | Empty first-main-entry marker: 0 means queued in reserved startup storage; -1 means wrong phase, cancellation or duplicate submission. Queueing does not establish STARTED. Trusted guests must make this their first main action; the host can check the armed phase, not prove the source-level calling position. |
| `error() -> i32` | Empty reporting-only diagnostic: 0 accepted, -2 FULL at four pending events, -1 outside authorized configuration/main or after cancellation. Identical empty events are retained as a bounded count; they cause no cleanup or outcome change. |
| `event_wait(timeout_us: i32) -> i32` | Immediate check at zero or cancellable monotonic wait up to 1,000,000 microseconds. This no-channel slice returns 1 for no event/timeout and -1 for invalid input, cancellation, shutdown or wait failure. It yields only the calling worker and also works during configuration. Channel notifications are future P4 work. |

The artifact must be ordinary WebAssembly version-1 bytecode of 1–65536 bytes,
with exactly one internal memory whose declared minimum and maximum are both
one 64 KiB page. Imports must be functions from the above list (at most three
entries); imported memory/globals/tables and nonempty tables are rejected.
The profile admits at most 64 defined functions and 16 exports. Export
signatures are checked after instantiation. Configuration is at most 64 bytes.

A start section and the implicit initialization exports `__post_instantiate`,
`__wasm_call_ctors` and `_initialize` are rejected. Required guest initialization
belongs in explicit `kaos_configure`; it must not be silently skipped. The
backend uses the classic interpreter with termination support; AOT/JIT, guest
threads, shared memory, multiple modules, libc/WASI, reference types and SIMD
are disabled. This is a rebuilt research workload profile, not a general WASI
application runner.

The backend pins WAMR revision
`b124f70345d712bead5c0c2393acb2dc583511de`. The build applies a narrowly matched
correction to a **build-directory copy** of its thread manager: final
execution-environment removal uses the cluster lock also used by concurrent
lookups, releasing it before cluster destruction takes the global list lock.
The dependency checkout is unchanged. Revision/source mismatches fail the
configuration so an upgrade requires review of the correction. Final
qualification of this corrected build is recorded separately from the model.
The build also corrects classic-interpreter frame/label alignment on x86_64;
both corrections apply only to generated build copies. Usage-aware allocation
routes guest linear memory through the same bounded allocator as runtime data.
These two additional corrections supersede the original host qualification
claims about clean UBSan output and complete arena accounting.

## Resource profile and event service

The host admission profile remains two resident nodes, four retained operation
records, 131072 variable bytes per node and 270336 total variable bytes. A
node's declared `requiredPeakBytes` includes its retained artifact:

```text
runtime arena bytes = requiredPeakBytes - artifactBytes
```

The host arena must be positive and cannot exceed 131072 bytes; the host node
quota also limits the artifact-plus-arena total to 131072. The allocator uses
a bounded per-worker arena (including zero-initialized linear memory), retaining internal allocations until whole-node
disposal. Loader temporaries, alignment overhead, instantiated memory,
configuration copying and runtime bookkeeping must all fit. WAMR is configured
with an 8192-byte instantiate stack, a 4096-byte application heap and an
8192-byte explicit execution environment, all within that arena. A valid
artifact can still fail preparation when its declared arena is insufficient.

Native infrastructure is separate: **two 262144-byte pthread stacks**, a
**32768-byte process-global WAMR arena**, fixed mailboxes, mutexes/condition
variables, control/transport records and native library/OS overhead. These
provisional Linux settings are not charged as each node's requested arena
and are not an ESP32 memory-feasibility claim. The generated sample guest uses
a 4096-byte C stack within its declared 64 KiB linear memory. For this fixed
one-page profile, pinned WAMR appends its 4096-byte application heap to the
backing allocation; that extra storage is also inside the charged arena.

INIT and completion state have fixed reserved mailbox storage. Worker wakeups
write to a nonblocking pipe; a full pipe already contains a wakeup, and losing
an additional byte cannot lose retained evidence. The host blocks in `poll`
on UDP, the runtime pipe and the earliest pending deadline. It services
control/cleanup before handling further datagrams and checks processing-time
expiry even for previously queued input. There is no idle busy loop or
operation-time worker join. Diagnostic stdout is nonblocking and may drop
output under backpressure; retained queryable results are the evidence path.
The four empty ERROR events use separate storage from INIT/completions.

## Lifecycle examples

For no-edge addition 41, instance identity is also 41. O authorizes its
immutable request; K admits it, pulls and validates the artifact, then hands
ownership to the runtime. Inert preparation publishes READY. The no-edge
predicate is true, so control records commit/start dispatch and requests
execution. After configuration succeeds, main calls `init()`. Control accepts
that marker only while the authorized instance is still active, nonclosing
and strictly before its original addition deadline. It then establishes the
node and records STARTED plus historical Succeeded. An artifact log or
`start_requested` flag is insufficient.

If configuration fails, main returns/traps before accepted startup, or the
addition expires, cleanup uses the original addition's authorization. A
queued INIT followed by exit cannot establish success: exit/usability and
acceptance are serialized. Full disposal gives FailedClean; missing closure
at cleanup cutoff gives Unresolved. Late disposal can resolve Unresolved,
but cannot turn late startup into timely success.

If main exits **after accepted success**, the creator report retains
Succeeded and adds exit/fault evidence as applicable. The instance becomes
unusable and safe local reclamation proceeds without waiting for O. That
local cleanup creates no new removal outcome or deadline. A fresh O removal
42 for exact instance 41 has its own record/deadline and succeeds only after
closure. Removing a running or still-preparing instance first fences it,
cancels pending/running work, drains accesses and follows the same disposal
path. Removing an already reclaimed instance can confirm closure. Reusing a
physical node slot preserves its nonwrapping lifetime-ticket sequence; old
tokens cannot act on the replacement. Operation/report records remain
retained under P2 rules; P7 storage-ACK/eviction work is still pending.

## Building and checking

From the repository root, with the pinned WAMR checkout in
`../sim_env/.deps/wamr` and WASI SDK clang at `/opt/wasi-sdk/bin/clang`:

```sh
cmake -S verification/graph_core -B /tmp/kaos-p3-build -G Ninja \
  -DCORE_BUILD_P3_HOST=ON -DCORE_BUILD_P2_HOST=ON -DCORE_SANITIZE=ON
cmake --build /tmp/kaos-p3-build
ctest --test-dir /tmp/kaos-p3-build --output-on-failure
ASAN_OPTIONS=detect_stack_use_after_return=0 \
  /tmp/kaos-p3-build/core/kaos_p3_host 58831 58830 1 99
```

The arguments are device UDP port, O UDP/artifact port, device identity and O
identity; optional `peer_id:udp_port` arguments follow. This host binds
loopback. Configure `CORE_WAMR_ROOT` or `CORE_WASI_CLANG` if dependencies live
elsewhere. The inspected guest compiler is WASI SDK clang 14.0.4; the build
uses `-nostdlib`, no entry point, explicit exports and fixed memory bounds.
CMake generates `node.wasm` for process tests and `runtime_guest.wasm` for
runtime-adapter tests.

ASan's fake-stack mode conflicts with this pinned WAMR version's native-stack
address checks. The `graph_wamr` test and P3 Python child host set
`detect_stack_use_after_return=0`; deterministic engine tests retain their
default sanitizer settings. Address, undefined-behavior and leak checking remain enabled, but stack-use-after-return
coverage is not claimed. Apply the same setting when manually launching an
instrumented runtime host.

From `../sim_env`, run the real O-to-C-to-WAMR process tests with:

```sh
KAOS_P3_HOST=/tmp/kaos-p3-build/core/kaos_p3_host \
KAOS_P3_GUEST=/tmp/kaos-p3-build/node.wasm \
  uv run --offline python -m unittest discover -s tests \
    -p 'test_graph_runtime_process.py' -v
```

`node_test.c` exercises deterministic engine ordering and resource guards;
`wamr_test.c` exercises the actual restricted runtime; the Python suite drives
real requests, artifact I/O and result queries. The final corrected build passed all 13 C suites with address/undefined-behavior
and leak checking (with the WAMR fake-stack exception above). Full Python
discovery ran 86 tests: 66 passed, including all nine new P3 process cases,
and 20 pre-existing opt-in integration cases were skipped. ThreadSanitizer
passed the runtime lifecycle cases and 200 concurrent cross-node
cancellation/disposal cycles after reproducing and fixing the upstream race.
One ThreadSanitizer launch failed before main with an address-mapping error;
the subsequent checked run passed without warnings. Commands, source hashes
and limitations are recorded in
[`p3_verification.json`](../verification/graph_core/p3_verification.json).

The [finite runtime model and handler mapping](../verification/graph_core/model/NodeRuntime.md)
passed TLC for two fixed identities: 496008 distinct states, nine invariants
and six properties. It covers reserved evidence, startup/exit/deadline races,
access drainage and disposal under explicit fairness/termination assumptions.
It does not prove WAMR memory safety, physical latency, slot reuse or separate
creator/removal report deadlines. Host runtime/sanitizer/process results
are recorded separately in the verification record linked above. The shared engine cross-build used
the existing `/tmp/kaos-p2-esp32` foundation project after loading the ESP-IDF
environment; its firmware fixture still runs foundation tests, not WAMR.
Those historical results preceded the target adapter and the accounting/alignment
corrections below. The foundation firmware remains separate; it is not the
firmware used for the new physical runtime qualification.


## ESP32 qualification and corrections

The new `verification/graph_core/espidf_p3` project links the same admission,
node engine, restricted artifact profile and WAMR worker implementation. It
embeds the existing runtime test guest; no existing test source was changed.
Its artifact staging is local and synthetic. The existing host UDP tests
remain the transport-to-runtime integration check.

The ESP32 profile uses two 16384-byte pthread stacks at priority 2, an
8192-byte control stack at priority 3, two arenas capped at 90112 bytes
(88 KiB), and the separate 32768-byte WAMR global arena. These are qualified
for the checked guest and no-edge fixture, not every admitted artifact or a
Wi-Fi deployment. Each request still pays for artifact bytes in addition to
its arena. Oversized/fragmented allocation failure remains ordinary preparation
failure with full cleanup. The worker and control priorities are part of the
port contract: control must preempt compute-only guests.

ESP-IDF 5.5 does not implement monotonic pthread condition attributes. Workers
remain pthread-created because WAMR uses pthread identity. Each target slot
instead has a static binary semaphore. A signal retains one wake token;
workers unlock the mailbox before waiting and recheck predicates after
reacquiring it. Timed waits use the monotonic clock and round up to RTOS ticks.
The semaphore is only a wakeup: commands, INIT and completions remain in the
reserved mailbox. Linux retains monotonic condition variables. The separate
[RuntimeWakeup model](../verification/graph_core/model/RuntimeWakeup.md) checks
this protocol; the existing NodeRuntime model and its scope still apply.

Physical bring-up found three issues, all corrected:

- The old WAMR configuration allocated linear memory through platform mmap,
  outside the node arena. `WAMR_BUILD_ALLOC_WITH_USAGE=1` now routes it through
  the same allocator and explicitly zeroes it. The new `wamr_budget_test.c`
  reproduces the old defect with 32/64 KiB budgets and passes after correction.
- ESP-IDF malloc only guarantees four-byte alignment. The arena and allocation
  headers now satisfy WAMR's eight-byte heap alignment requirement.
- The pinned classic interpreter under-aligned pointer-bearing frames and
  label stacks on x86_64. Exact-match build-copy corrections preserve pointer
  alignment. The previous host binary also reproduced this UBSan error;
  previous CTest success did not establish a clean UBSan run. Sanitizer builds
  now fail immediately on undefined behavior.

The initial 128 KiB arenas could not fit either contiguous target heap block.
With full admission/engine records linked, two 96 KiB arenas also failed.
The 88 KiB profile passed two-resident execution, two simultaneous compute
loops, bounded-import/configuration cancellation, inert preparation, early
normal/trapping exits, expired and cancelled entry, partial preparation failure
and 20 complete reuse/reclamation cycles. Real engine cases passed addition,
removal, pre-start failure, expiry and tracked-access cutoff followed by late
cleanup. Queryable evidence and resource-release assertions use the real core.

Build and run on the target (this flashes the qualification firmware):

```sh
. /home/karolina/esp/esp-idf/export.sh
idf.py -C verification/graph_core/espidf_p3 -B /tmp/kaos-p3-esp32 \
  -p /dev/ttyUSB0 flash monitor
```

The target requires original ESP32, no PSRAM, assertions and FreeRTOS trace
support for WAMR stack-boundary checks. The fixture prints heap free/minimum/
largest-block values, task stack high-water marks and control/cleanup timing.
It leaves the board running the qualification firmware. Measured values,
firmware/source hashes, commands, failed bring-up attempts and final logs are
in [p3_target_verification.json](../verification/graph_core/p3_target_verification.json).

This closes the P3 no-edge implementation and target lifecycle qualification.
It does not establish two-node/one-channel Wi-Fi feasibility, distributed radio
behavior, hard worst-case timing, collector/ACK/eviction integration, reboot
recovery or P4–P9 completion. Those broader R05/Q14 gates remain open. The
measured stack and timing values are observations for this finite workload.


## Four-node capacity experiment

The user now requires at least four nodes for viability and asks to target eight
endpoints per node. This is a new capacity goal; the qualified production
limits above remain unchanged. An isolated build-copy experiment is staged by:

```sh
python3 verification/graph_core/capacity/stage.py /tmp/kaos-capacity-run
. /home/karolina/esp/esp-idf/export.sh
idf.py -C /tmp/kaos-capacity-run/project -B /tmp/kaos-capacity-run/build \
  -p /dev/ttyUSB0 flash monitor
```

Only the copy increases node slots to four, endpoint records to 32, operation
records to eight, ports/incidence to eight and object facts to 17. The probe
bypasses protocol admission; these changes reserve a candidate registry layout
and do not establish that larger requests/reports fit the current wire bounds.
The normal graph core limits and existing test artifacts are not changed.

The guest is the existing lifecycle fixture rebuilt with
`--export=__heap_base --export=__data_end`. Pinned WAMR already enables shrunk
memory. When it recognizes linker-generated data/stack bounds and finds no
`memory.size`/`memory.grow`, it can shrink the physical linear backing. This
is appropriate for compiler-produced guests that respect that layout, not a
guarantee of all accesses allowed by an arbitrary declared 64 KiB memory.
The 4 KiB guest C stack, 8 KiB interpreter stack, 4 KiB app heap and 16 KiB
ESP32 native worker stack remain unchanged.

| Measurement | Original guest | Compact guest |
| --- | ---: | ---: |
| Artifact bytes | 626 | 665 |
| Host minimum successful arena (8-byte search granularity) | 86080 | 28976 |
| ESP32 minimum successful arena in this experiment | Not remeasured | 25184 |
| Arena used for four-node experiment | Not feasible with existing 88 KiB profile | 25600 |

The target starts all four guests (two compute-only, two waiting), then cancels
and disposes each, for 20 cycles. It allocates and touches 32 separate 1024-byte
endpoint-budget buffers; each represents the current four-message/256-byte
quota. Candidate registry storage is 16936 bytes. All four guests running with
those reservations left 41448 free internal 8-bit heap bytes; the largest free
block was 15872 bytes. Each cycle returned to its warm free-heap baseline.
This establishes four-runtime residency plus reserved memory, not 32 working
endpoints, a maximum node count, or Wi-Fi deployment feasibility.

Beacon's existing embedded artifacts supply useful workload scale:

| Artifact under `../beacon/modules` | Binary bytes | Linker heap base |
| --- | ---: | ---: |
| `evaluation_A.wasm` | 593 | 5168 |
| `evaluation_B.wasm` | 460 | 5136 |
| `button_module.wasm` | 568 | 5232 |
| `press_light_demo/button_module.wasm` | 3371 | 5488 |
| `press_light_demo/led_module.wasm` | 3557 | 5424 |

These bounds already include their approximately 4 KiB C stacks, but not
host-managed heap, WAMR metadata/interpreter stacks or native task stacks.
The Beacon modules use old `env` imports and setup/main exports, and their
memory declarations lack the explicit maximum required by P3. They were
inspected, not rebuilt or executed by this probe. Heap-base metadata alone
also does not prove eligibility for shrinking: memory instructions and
runtime allocation demand matter. Some examples allocate channel descriptors
and received messages dynamically.

The sizing sweep exposed a pinned-WAMR bug: failed temporary-vector allocation
left its lock uninitialized before the destruction path. The exact-match
build-copy correction initializes the vector to zero; the ESP32 sweep then
passed. The no-guest-thread profile is important: this does not claim recovery
from failed traversal of a cluster containing other guest threads. A new host
capacity regression covers the compact guest, budget sweep and repeated
concurrent disposal. All 15 host sanitizer suites pass; existing tests remain
unchanged.

Do not adopt the measured 25184-byte threshold as a universal node quota.
Select an arena from the actual guest's peak, including configuration and
cleanup, with headroom. Four nodes with eight endpoints still require P4
traffic tests, full queues, actual Beacon-like workloads, Wi-Fi and management
storage, safe cleanup under pressure, and checked enlarged protocol bounds.
A four-input/four-output transform with small messages is a useful first
workload. Smaller heaps/stacks are additional experiments; no such reduction
was needed to obtain the result above.

Commands, source/firmware hashes and logs are in
[`capacity/results.json`](../verification/graph_core/capacity/results.json).
The board is left running the capacity experiment firmware.
