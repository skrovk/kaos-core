# Graph operation implementation plan

Terminology: [shared glossary](graph_operation_glossary.md).

This plan designs the six graph operations from the thesis protocols and finite verification for a small embedded device, with evaluation in `../sim_env`. Existing feature implementations may be replaced completely. The proposed starting point is a new bounded protocol core, explicit runtime and endpoint ownership, and shared host/embedded interfaces. Reuse is justified by protocol fit, measurable resource cost and testability; existing code structure is not a constraint on the design.

Prepared on 1 October 2026; consolidated on 2 October 2026 against all recorded answers Q01–Q23 in the [design questions](graph_operation_design_questions.md). Agreed answers are implementation inputs; provisional choices and unanswered policies remain explicitly marked. The [common principles summary](graph_operation_design_principles_summary.md) distills the agreed baseline; the earlier [detailed principles](graph_operation_design_principles.md) summarize Q01–Q07 only and must be read with the later answers. The [test catalogue](graph_operation_test_plan.md) defines concrete stimuli and expected observations independently of legacy compatibility. The [verification inventory](graph_operation_verification_inventory.csv) maps every existing configuration to planned implementation regressions and records its audit scope. These documents do not change runtime code.

The initial implementation sequence began with bounded core, transport and WAMR qualification prototypes. See [current status](#current-status-and-verification-baseline) for completed work and the latest verification baseline. Freezing the complete ABI, evidence schema, resource profile and ventilation behavior requires the specific deliverables in [Remaining information and implementation gates](#remaining-information-and-implementation-gates). Missing measured limits are outputs of those prototypes, not a reason to postpone them. This plan does not treat advancing past a discussion question as approval of its unanswered parts.

Scope decision on 2 October 2026: [Q05](graph_operation_design_questions.md#q05) accepts obsolete construction after required rejection history is discarded as an implementation limitation. A complete permanent-retention, compaction or fresh-confirmation mechanism is deferred. Stale ADD/PREPARE may recreate an object; resource/peripheral/data effects and possible O reconciliation must be disclosed. Source protocols and formal results remain unchanged and stronger in this respect. Statements below about no resurrection, at-most-once execution and safe retirement are reference goals subject to this explicit exception, not unconditional implementation claims. Checks against live/pending records, bounded resource admission, outstanding cleanup and memory-safe ownership remain required. Periodic O sweeps are a possible mitigation, not yet a committed feature.

## Current status and verification baseline

Current status through **23 October 2026**, based on recorded graph-operation
decisions, the application proposal and implementation evidence.
A missing test or measurement is not automatically a missing policy decision.

The behavior-preserving simplification, formatting and repository cleanup are
complete. The latest cleanup verification passes **60/60 default** and
**34/34 four-node** sanitizer entries. The next protocol change is application
traffic over CoAP, pending its traffic/wire contract; fault experiments, formal
coverage and target qualification remain open independently of that decision.

| Work | Completed scope and remaining boundary |
|---|---|
| Native imports and application fixtures | Bounded per-instance grants, a fixed trusted registry, signature/memory checks, cancellation and replacement isolation; APP03 optional-branch expiry; APP11 EMPTY/FULL/drop plumbing with real WAMR guests. The sensor remains synthetic; application validity and fallback still need R07 decisions. See [continuation evidence](../verification/graph_core/continuation_verification.json). |
| Lifecycle and fault fixtures | Configuration/main-entry, adapter completion and real-WAMR preparation/destruction barriers; observation-corruption rejection; live relay-overflow accounting; collector ACK-boundary crashes and shared-host crashes. These cover selected schedules, with remaining gaps listed under [agreed work](#agreed-work-that-can-proceed). |
| Two-port transport scheduling | Separate application/management sockets, bounded rounds, recurring management service and paced artifact transfer; simulator routing shares one directed-link impairment policy and pending-copy bound. [Transport evidence](../verification/graph_core/transport_scheduling_verification.json) includes exploratory contention/checksum runs and an ESP32 cross-build, not target timing acceptance. |
| Simplification, steps 0–6 | Shared UDP beside CoAP under `graph_core/`; reused codec/validation helpers; explicit admission, host and teardown phases; raw/decoded control equivalence and worker-startup unwind coverage. Initial node cleanup, exactly two edge-then-node passes, receiver-first activation and original deadlines are preserved. [Evidence](../verification/graph_core/simplification_verification.json). Only protocol slice P remains pending in this plan. |
| Platform layout and readability | Clocks reside in `ports/posix/` and `ports/espidf/`. [Formatting guidance](../AGENTS.md#c-formatting-and-readability), pinned clang-format configuration, semantic statement grouping and equivalent guard clauses are implemented. Existing test sources were preserved. [Formatting evidence](../verification/graph_core/formatting_verification.json). |
| Repository cleanup | Legacy root implementation, its tests/build output, unused unicoap and P3/P4 executable aliases are retired. Use `CORE_BUILD_HOST` / `kaos_host`; retain the P2 regression fixture. The sibling simulator reports the retired build path. Historical contracts/results remain indexed at their original paths. This did not port legacy application features. [Cleanup record](repository_cleanup.md). |

The latest [cleanup verification](../verification/graph_core/cleanup_verification.json)
records **60/60 default** and **34/34 four-node** sanitizer entries, a successful
standalone core build without WAMR, and both simulator retirement checks.
All 758 pre-existing verification files were preserved except the two CMake
files updated to select the canonical host option; test sources and saved
evidence were unchanged. The cleanup did not rebuild ESP-IDF or run hardware.

The preceding simplification and formatting passes also ran **9/9 opt-in runtime
process cases** and passed the ESP32 four-node network-host cross-build. These
are separate recorded runs, not additional cases rerun during cleanup. Default
Python discovery retains opt-in skips, and the smaller profile skips its
three-node local pipeline; passing suite totals do not establish full coverage.

Earlier increments and exact coverage remain in the
[adapter-barrier](../verification/graph_core/adapter_barrier_verification.json),
[observation-loss](../verification/graph_core/observation_loss_verification.json),
[collector-crash](../verification/graph_core/collector_crash_verification.json),
[relay-overflow](../verification/graph_core/relay_overflow_verification.json),
[host-crash](../verification/graph_core/host_crash_verification.json) and
[WAMR-barrier](../verification/graph_core/wamr_phase_barrier_verification.json)
records. Their historical test totals are not the current baseline.

## Evidence and source precedence

| Reference | Source and use |
|---|---|
| SH | [Shared mechanisms](../../../Thesis/protocol%20specs/graph_operation_shared_mechanisms.md), M1–M7: common admission, evidence, preparation, activation, startup, closure, reporting and deadlines. |
| V2 | [Node addition v2](../../../Thesis/protocol%20specs/node_addition_protocol_v2.md): multiple channels, predicates, F0–F15/C1–C6, A1–A9/E1–E6 and conditional P1–P7/L1–L4. |
| IN | [Implementation notes](../../../Thesis/protocol%20specs/graph_operation_implementation_notes.md): concrete services must substantiate interface assumptions; no extra protocol states or acknowledgements. |
| OP | [Node addition without edges](../../../Thesis/protocol%20specs/node_addition_without_edges_protocol.md), [edge addition](../../../Thesis/protocol%20specs/edge_addition_protocol.md), [node removal without edges](../../../Thesis/protocol%20specs/node_removal_without_edges_protocol.md), [node removal with edges](../../../Thesis/protocol%20specs/node_removal_with_edges_protocol.md), [edge removal](../../../Thesis/protocol%20specs/edge_removal_protocol.md). |
| VN | [Node verification report](../../../Thesis/verification/node_addition/README.md), [walkthrough](../../../Thesis/verification/node_addition/WALKTHROUGH.md), models/configurations/logs: Boolean contracts and single-operation/endpoint behavior. |
| VG | [Graph verification report](../../../Thesis/verification/graph_operations/README.md), [verification plan](../../../Thesis/verification/graph_operations/PLAN.md), models/configurations/logs: overlapping lifecycles, admission, scheduling and reuse. |
| KD | [KaOS design](../../../Thesis/kaos_design.tex): ports, manifests, authority boundaries, channel semantics and candidate application interfaces. |
| FT | [Fault tolerance](../../../Thesis/fault_tolerance.tex): selected ventilation example, fault categories, evaluation requirements and open service criteria. |
| OTHER | [Orchestrator design](../../../Thesis/kaos_orchestrator.tex), [design v2 notes](../../../Thesis/kaos_design_v2.tex), [formalisation](../../../Thesis/formalisation.tex), [main thesis](../../../Thesis/main.tex): compatible architectural decisions and historical proposals. |
| SIM | [Simulator README](../../sim_env/README.md), Python runner/executor, native port and host patch, scenarios/workloads/tests: implementation evidence, not protocol requirements. |

Use SH/V2/OP for current graph-operation semantics. VN/VG state exactly what was checked, not stronger guarantees. Preserve compatible decisions elsewhere, but label unresolved proposals. The older [node addition plan](../../../Thesis/protocol%20specs/node_addition_protocol_plan.md) is historical where its acknowledgement/finalization phases disagree with V2. Do not introduce those phases into the implementation.

For this implementation's selected scope, explicit recorded answers take precedence over earlier proposals. The [conflict register](#conflicts-and-reconciliation) identifies deliberate differences from SH/V2/OP; it does not rewrite those reference protocols or their verification results. Within the discussion record, use the final agreed answer and subsequent explicit resolutions, especially Q18 for Q11's data calls and Q23 for Q12's runtime qualification.

The working copies matter: Thesis and sim_env contain pre-existing modified/untracked work. Their HEADs alone do not identify the reviewed content. At inspection, HEADs were `d71ed1a` (kaos_core), `cca31dc` (sim_env), and `a548d0e` (Thesis). Preserve that work and capture content hashes with subsequent implementation test runs.

## Implementation freedom and reuse criteria

Backward compatibility with Beacon LOAD, existing guest binaries, current message encodings, internal structures and source layouts is not a requirement of this plan. The six operations can be exposed through a new API, applications can be rebuilt for a new guest ABI, and the simulator's executor, transport integration and observer can be replaced as needed. There is no mandatory legacy adapter, dual protocol stack or in-place upgrade mechanism. Switching a running deployment with retained obligations would require a separately specified transition; laboratory evaluation can start each new implementation from an empty deployment.

| Design input | What carries forward | What may be replaced |
|---|---|---|
| Thesis and verification | Operation semantics, evidence/ownership rules, documented decisions, failure cases and the limits of finite checking. | Concrete state layout, source files, synchronization, codecs, allocators and runtime integration. Replacing code does not change the protocol or expand its verified scope. |
| Application model | WebAssembly logical nodes, declared ports/capabilities, placement constraints and workload meaning. | WAMR wrappers or engine, guest entrypoint names, import ABI, manifest encoding and compiled fixture binaries. |
| Embedded deployment | Bounded RAM/flash/CPU, independently progressing control work and the selected device profile. | Existing Kconfig defaults, queue types/depths, thread/task structure and per-channel allocation strategy. |
| Evaluation in sim_env | Run actual implementation code, inject faults, preserve attributed evidence and reproduce experiments. | LOAD executor, native host patch, command framing, telemetry schema and scenario syntax. Preserve relevant scenario intent and measurement integrity, not obsolete API behavior. |

For each candidate component, record **reuse unchanged**, **adapt**, or **replace**, supported by its contracts, footprint, ownership and conformance tests. Missing fundamental identity, permission or lifetime guarantees favor replacement over layering new state onto an incompatible component. Small transport, clock, logging or codec utilities can still be reused where their interfaces and bounds fit. A replacement does not require first repairing the discarded implementation; turn useful bug examples into implementation-independent regression tests.

Establish a minimal shared build for the new core early. Link that core into host and embedded ports with explicit interfaces rather than relying on a patch to rewrite core behavior for simulation. The existing host patch is a source of lessons and regression cases; reproducing or upstreaming every patch hunk is not a prerequisite for replacement.

## Existing decisions to carry into the implementation

| Decision | Implementation consequence | Source |
|---|---|---|
| Original ESP32 LILYGO TTGO T-Display, no PSRAM, Wi-Fi only; four resident WASM nodes are the viability minimum, with eight endpoints per node the desired capacity target (user update, 9 October 2026). | Establish finite provisional limits immediately; measure usable RAM/flash, stack and peak lifecycle overlap on the actual board before claiming supported capacity. Qualify a separate distributed placement for Wi-Fi channel claims. Pico and emulation are optional extensions. | [Q01](graph_operation_design_questions.md#q01) |
| Crash-stop baseline for devices and O; temporary disconnection with retained state is supported. | Existing applications and admitted work continue under their guards/deadlines during management outages. No reboot/rejoin, allocator restart into the live system or O takeover. Isolate traffic/authority between fresh laboratory runs. | [Q02](graph_operation_design_questions.md#q02) |
| O allocates one non-recycled unsigned 64-bit opId sequence across devices and deployment workflows. | Instance ID equals its creating node-addition opId with a distinct API type; a binding is `(channelId, creatingOpId)`. Later removals have their own opId and exact target. Checked exhaustion refuses fresh allocations, including new removals; existing authorized work continues. No extra deployment UUID, pipeline ID or allocator. | [Q03](graph_operation_design_questions.md#q03) |
| O authorizes graph changes; Kc coordinates one operation; each Ki owns its local objects. | Local services may be separate tasks, but remain one protocol entity per host. An application observation/request does not grant remote authority. | SH §1; FT responsibilities; KD interactions |
| Logical node, concrete instance and physical device are different identities. | Keep binary/manifest identity, instance ownership, and placement separate. Resources and runtime history belong to the instance. | KD system model; V2 §2 |
| `channelId = (sourceLogicalNodeId, destinationLogicalNodeId, serviceId)` is directed and globally distinguished by the full tuple. | Multiple services on one node pair and one service on disjoint pairs are valid. Instance/port/placement are descriptors, not a replacement for channel identity. | V2 §2; SH §2 |
| One preparing/active/removing binding occupies a channel identity. | Known unresolved work retains ownership; a fresh removal identity targets the original creation binding. Preserve identity isolation for retained/new bindings. Permanent prevention of stale construction after rejection history is discarded is the explicit Q05 gap. | M1/M6, E3/E5; Q05 exception |
| P is a fixed monotone dependency predicate over complete channels. | Same predicate at commit and startup; validate `P(Channels)`. Failure does not change declared group totals; no minimum-cardinality optimization is required. | V2 §3; VN BooleanContracts |
| Node and endpoint preparation are independent. | Reserve registry/ports before guest execution; installing inactive associations must not require running N. Preserve mappings during runtime initialization. | M3; IN |
| Commit grants no execution or data permission. | Only receiver activation opens Receive/Get; accepted receiver ACTIVE permits sender activation and Put/Send. Local channels obey the same ordering. | M3–M5 |
| Dispatch, execution, STARTED emission and acceptance are distinct. | Recheck guards at execution; start at most once. Missing confirmation does not prove that no application effects occurred. | M5; V2 F9–F12 |
| Startup success is historical and terminal. | Success does not wait for O or peer outcome acknowledgement, all-channel join or first output, and provides no ongoing-health guarantee. Q06's acknowledgement of evidence stored by O or the designated DB controls local report reclamation only. Pending optional work retains owners/deadlines after success. | V2 §§5,7–9; SH M7; Q06 |
| Cleanup includes every selected object, even never-ready objects. | Failed node addition cleans N and both endpoints of every declared channel. Node removal uses complete current incidence; edge cleanup preserves existing runtimes. | M1/M6 |
| Reference CLEANED means terminal cancellation, disabled access, safe lifetime, and released owned resources/associations. | Establish that local work has stopped and release resources; stopping a thread or deleting a queue alone is insufficient. Selection at Kc does not immediately revoke remote permission. Report the Q05 gap in permanent cancellation after history is discarded rather than claiming full M6 conformance. | M6; IN; VG registry counterexample; Q05 exception |
| Operation outcomes are Pending, Succeeded, FailedClean and Unresolved. | Rejection precedes acceptance and is not a fifth accepted-operation outcome. Incomplete requested removal remains Pending/Unresolved, never FailedClean. | M1/M7 |
| Commit and dispatch history are monotone facts. | Retain `commitRecorded` and `startDispatched` or an equivalent representation; derive Abort, CommitNoStart or StartIssued. Do not independently mutate a redundant failure-history state. | VN walkthrough; SH M7 |
| Deadlines belong to local monotonic clocks, with fixed start events. | Retrying, querying, commit and startup never reset allowances. Ki cannot read Kc time and does not silently expire prepared reservations. | M7, A9/P5 |
| Capacity includes partial allocations, outstanding actions and retained results. | Q06 permits pressure-triggered eviction of the oldest eligible Succeeded/FailedClean report history after reclaiming eligible acknowledged copies; live/pending state and unresolved obligations remain protected. Defer admissions requiring capacity that cannot be reclaimed without discarding protected state. Detailed result retirement and old-command rejection are separate policies. | A8/E6; SH M7; IN; Q06 |
| Endpoint lifecycle, communication health and application service are separate observations. | Put acceptance, transport receipt, Get, application acceptance and actuator execution are different completion boundaries. | V2 §8; KD contracts; FT |
| WASM tasks use authorized ports through KaOS; peripheral placement constraints matter. | Preserve instance-scoped authority and hardware requirements. Spare compute cannot substitute for a missing physical sensor or actuator. | KD/FT |
| The selected application is room ventilation. | Model sensing, validation/aggregation, supervisory policy and a device-local feedback/actuation loop. Movable computation and hardware-bound tasks are distinct; this does not implement migration. | FT representative application |

### Older statements requiring reconciliation

| Older wording | Treatment in this plan |
|---|---|
| `Unbound/Bound/Paused`, receiver HELLO handshake, and channel generation in KD/OTHER | Map current construction to Prepared/Active and explicit role permissions. An optional transport handshake may implement interface validation; it cannot replace M4 or create a new universal protocol exchange. Q03 selects `(channelId, creatingOpId)` for protocol binding isolation; an internal lifetime token remains an implementation choice under Q13. |
| Possible application readiness response in KD | Preserve dynamic-interface validation and any platform-specific binding hook, but M3 expressly adds no universal application READY handshake. |
| O feasibility/reservation milestones and old peer outcome acknowledgements | O may keep observations, but Kc decides from attributed evidence; no extra success-finalization phase. |
| Atomic insertion, migration, replacement and pause/resume examples | V2 does not promise atomic graph visibility or in-place rebinding. Migration/state transfer and autonomous recovery need separate specifications. |
| V2 §11 suggests querying current state as an alternative to retained reports | SH M7 explicitly requires retained attributed outcomes and bounded admission. Current-state queries cannot reconstruct historical commit/start/cleanup evidence. Keep queries and define retention policy. |
| Threshold shorthand in V2 versus Boolean-only revised verification | Q10 agrees to Boolean-only public support: channel references, true/false and and/or. Reject atLeast/percent shorthand; threshold arithmetic/equivalence work is deferred. |
| Main thesis draft uses 4-second sampling/12-second age examples | FT explicitly says numerical ventilation limits have not been selected. Treat draft numbers and simulator defaults as illustrative, not approved requirements. |
| Derived files still say “not independently verified” | Report VG's newer finite evidence with its actual bounds; do not silently promote that evidence to an implementation or unbounded proof. |

## Operation completion and failure scope

| Operation and coordinator | Acceptance and construction | Success evidence | Failure or removal obligations |
|---|---|---|---|
| Node addition with channels; N host | Fixed manifest, valid P, instance/incident reservations; independently prepare runtime and endpoints, commit, activate sufficient channels, dispatch once. | Accepted STARTED for N with P(ActiveSet), local usability and unexpired node deadline. | N plus both endpoints of every declared channel. Channel failure alone cleans its pair; before dispatch, continue if P remains possible; during Starting, known loss of P(ActiveSet) selects whole cleanup. |
| Node addition without edges; N host | Established empty incidence, empty manifest, P=true; prohibit incident admission until success or confirmed cleanup. Prepare/commit/dispatch still apply. | Accepted STARTED; empty channelStatus. | N, including cancellation against a delayed creation. No endpoint or channel deadline. |
| Edge addition; source host | Both node additions have succeeded; all nodes support dynamic binding; reserve fresh binding, prepare endpoint pair, commit, activate receiver then sender. | Accepted ACTIVE for both endpoints before channel deadline. No node STARTED gate. | Both endpoints, including never-ready peer; keep both runtimes/unrelated channels. Abort/CommitNoStart history applies. |
| Node removal without edges; N host | Establish complete empty incidence at acceptance and fence new incident work; N may be preparing, running, failed or cleaning. | CLEANED(N), including registry lifetime. | N only; Pending/Unresolved until closure, then Succeeded. Unknown/nonempty incidence rejects this specialization. |
| Node removal with edges; N host | Establish complete current incidence, including authorized/pending/failed/unresolved bindings and optional work; fence admission. | CLEANED for N and both endpoints of every incident binding. | Fixed full scope, independent cleanup branches, no recreation or rollback. Prior successful additions remain historical. |
| Edge removal; source host | Established node instances; match concrete binding and original creation, coordinate pending edge setup, fence construction. | CLEANED for both endpoint roles, including co-located roles. | Preserve nodes and unrelated edges. The pending creating edge addition fails independently; removal may succeed. |

For failed additions, cleanup is Pending while collecting evidence, FailedClean after all closure, and Unresolved at cutoff with explicit missing owners/objects. Late complete evidence advances Unresolved to FailedClean. For requested removals, complete late evidence gives Succeeded. A channel's unresolved cleanup does not rewrite a parent node's previous success.

## Current implementation and environment findings

These source-level findings and earlier checks identify hazards and evaluation gaps. They are not a required backlog of repairs to the old code. Each response can be implemented through replacement, and defects in discarded components need not be fixed first.

| ID | Evidence and issue | Required plan response |
|---|---|---|
| ENV01 | [Simulator executor](../../sim_env/sim_env/simulator.py) and README support globally serialized LOAD only; failure/timeout halts dispatch. Core has legacy load/reload/suspend/destroy states, not the six graph protocols. | Define the new six-operation API and implement a concurrent simulator O client against it. Replace the LOAD executor as needed; asynchronous results, queries and reconciliation must not inherit its global halt/serialization rules. |
| ENV02 | [Monitor header](../include/kaos_monitor.h) uses `int8_t op_id_t`; [monitor](../kaos_monitor.c) recycles 10 operation slots and clears records at removal. Main event queue capacity is 10. | Separate slot handles from stable operation/instance identities; reserve record capacity; protect delayed events from slot reuse; guarantee control-event handling under saturation. |
| ENV03 | [Container manager](../container_mgr.c) `init_module` runs guest `setup`, then monitor startup proceeds to `main_app`; `execute_function` sets RUNNING before the WASM call. [Signals](../kaos_signals.c) maps guest liveness signal 0 to EVENT_CONTAINER_LIVE, used for LOAD completion. | Use inert preparation and guarded entry; Q12 selects the existing empty signal at main entry as host-attributed STARTED evidence under a trusted-workload convention. Adapt identity, phase checks and bounded queue handling; a pre-call RUNNING flag is insufficient. |
| ENV04 | [Unreliable channel](../unreliable_channel.c) creates queues lazily in Put/Receive, without Prepared/Active/removal gates or creation-incarnation fields. Public receive entry takes a buffer without its length. | Design endpoint storage and a bounded data interface around authorized preparation, role permissions, explicit input lengths and creation isolation. Existing queue and packet representations need not survive. |
| ENV05 | [Simulator README](../../sim_env/README.md) says channel telemetry reports queue existence with endpoint state `unknown`. Snapshots describe deployment rather than protocol evidence. | Emit actual lifecycle transitions, issued/accepted evidence, original authorizations, missing closure obligations and deadline ownership. Unknown evidence cannot satisfy a gate. |
| ENV06 | Reliable direct TCP deployment bypasses the impaired datagram relay. The relay supports directed loss, delay/jitter and outage, but no explicit per-message duplication or protocol-boundary scheduler. | Impair O↔Kc and Kc↔Ki control traffic as well as data; add deterministic hold/drop/duplicate/reorder and local-event barriers. Current network scenarios cannot cover F1/F13 or all cleanup races. |
| ENV07 | POSIX threads do not reproduce FreeRTOS priority, native stack sizes, interrupt execution, ESP heap behavior or radio contention. Shared host time can hide device-clock differences. | Add constrained allocator/configuration profiles and virtual clocks for logic tests; separately measure embedded execution, memory and stack bounds. Host timing does not certify an ESP32. |
| ENV08 | Host patch changes parser, lifetime/queue paths, initialization, operation argument ownership and observation behavior. The ESP build does not automatically inherit these fixes. | Compile the new shared core unchanged in both ports. Retain relevant patch findings as regressions; only retained code needs patch reconciliation. Remove the patch dependency for replaced components. |
| ENV09 | [Core CMakeLists](../CMakeLists.txt) names `network.c` (absent here) and `cbor_composer` without the checked-in `.c` filename. Host CMake builds a different explicit source set. | Create or replace build definitions for the selected architecture and ESP-IDF integration. Correct the old list only if reused; successful new host/embedded builds are the acceptance criterion. No embedded build was attempted in the earlier audit. |
| ENV10 | Build validation in [runner](../../sim_env/sim_env/runner.py) checks the compiled copied core and host files, but not the live upstream source tree against a fresh patch result. | Record exact new core, port, dependency and artifact hashes; reject stale builds. The new build may eliminate source copying and patching entirely. Keep a regression for running an old artifact after its input changes. |
| ENV11 | Version 2 has no workload evaluator for deadline/freshness; ventilation plant, semantic sensor faults, application command outcomes and device-level memory faults are absent. | Add the fixtures and metrics specified below. Keep protocol conformance and application service results distinct. |
| ENV12 | Seeded relay decisions and event replay do not make OS-thread execution deterministic. Fault schedules are relative to measurement after warmup, rather than protocol phase. | Add barriers keyed to lifecycle events, deterministic unit scheduling and trace shrinking; retain seeded process experiments for integration and distributions. |
| ENV13 | Reboot starts an empty process with a new boot ID, while protocol verification is crash-stop. Process exit erases volatile allocations but does not provide protocol CLEANED. | Treat reboot as an extension/negative boundary test; never count process termination as all required closure evidence or safe reuse. |
| ENV14 | Queue destruction in the host port requires no concurrent users; timers cannot delete themselves inside callbacks. Core uses pointers, mutexes and variable-sized allocations. | Make cancellation and the stopped state explicit across port APIs; test callbacks and queue consumers still in flight at removal. |

### Resource configuration differences

These are inspected defaults in [Kconfig](../Kconfig.projbuild) and [host.h](../../sim_env/native/include/host.h), not measured available hardware capacity or requirements for the replacement. Select new bounds from workload demand and the target memory/processing ledger, then expose the same profile to both ports.

| Limit | Embedded default | Simulator host |
|---|---:|---:|
| Resident modules | 5 | 32 |
| Entries in each identity/input/output list | 10 | 32 |
| Resource declarations | 20 | 32 |
| Module name bytes excluding terminator | 20 | 63 |
| Queue items | 32 | 32 |
| Guest setup execution stack setting | 1,024 bytes | 16,384 bytes |
| Artifact buffer bound | 4,096 bytes | 1,048,576 bytes |
| Timers per module | 5 | 5 |
| Legacy suspend timeout | 10,000,000 µs | 1,000,000 µs |

Simulator example modules request 65,536-byte runtime stack and heap each, while their compilation uses a separate 16 KiB linear stack. These settings describe different allocations and must be measured rather than summed blindly. Even two 64 KiB per-instance reservations would be 128 KiB before module/runtime/queue/native-stack overhead; their suitability for the selected no-PSRAM T-Display is unqualified.

### Evidence from the initial planning audit

The following checks describe the inspected implementation before replacement. They were performed when the initial plan was prepared, not rerun for this documentation revision. They provide a reference and do not impose compatibility gates on new code.

- VG read-only `run_checks.py --audit`: **55/55** current input and retained-log matches. Its reported results comprise 29 positive checks, 16 expected negative counterexamples and 10 witnesses.
- VN independent hash comparison: **35/35** current model/dependency/configuration hashes match saved summaries; all entries say expected result matched (19 positive, 7 negative, 9 witnesses). This older summary does not hash protocol text, runner or logs; it is a narrower audit than VG.
- Simulator `.venv/bin/python -B -m unittest discover -s tests -v`: **41 discovered, 21 passed, 20 process/socket integration tests skipped**. Native `build/port-test`: exit 0.
- Existing build-manifest hashes: **4 artifacts, 16 host inputs and 33 copied-core inputs match**. `patch --dry-run --batch --fuzz=0` applies the current host patch to this core without conflict.
- `uv`, CMake, C compiler, patch, Java, `/opt/wasi-sdk/bin/clang`, and an ESP-IDF export script are present. Presence is not proof of an embedded build or hardware readiness.

No TLC exploration, native rebuild, process/socket integration run, sanitizer run, or embedded execution was performed for this documentation task. Existing dirty Thesis/simulator work was preserved. Subsequent simulator builds/runs must use a writable output/build location and permit local sockets/process signals; those environment requirements are documented in SIM.

## Replacement architecture and agreed contracts

Design a platform-independent protocol core with typed events, explicit owned records and effect interfaces. A proposed transition function consumes a validated event and a local time observation, updates the relevant operation/object records at a consistent decision boundary, and issues bounded work requests. Runtime work, endpoint preparation, transport delivery and timer handling remain separately observable actions. This does not make an entire operation atomic or require a single worker for all device activity.

Host tests can control these effect interfaces deterministically; the process simulator and embedded port must execute the same production transitions. Runtime/endpoint workers complete through attributed events, while data access enforces endpoint permission and lifetime through a bounded API. Q13/Q14 select bounded copying, tracked accesses and short synchronization around admission versus cleanup; concrete locks and record layouts remain implementation choices. Routing every payload through one management event queue is not required.

O and K have separate implementation responsibilities. Shared protocol definitions describe identities and messages that both roles must understand; sharing those definitions does not give K authority to allocate operation IDs or authorize graph changes. In particular, `core_op_id` and `core_instance_id` describe identities used by both roles, while the single non-recycled operation-ID sequence and its allocator belong exclusively to O.

O's implementation owns operation-ID allocation, authorization, immutable requests, complete intended incidence bookkeeping/fences and concurrent submission/reconciliation. K's implementation owns local admission and resources, Kc coordination and Ki object transitions, worker execution, evidence acceptance and cleanup under O's authorization. The `core_lifetime`, `core_access` and `core_action` utilities support K's local execution machinery. K does not allocate fresh graph-operation IDs; internal access tickets are separate local lifetime identifiers.

The plan includes operational logic for both roles, with O implemented separately from K's C execution library. The simulator's Python O client is an allowed implementation location; it must exercise the actual K protocol implementation rather than substitute simulator decisions for device transitions. Keep O's operational state and allocation logic out of K's firmware/core. The initially misplaced `core_next_op_id()` implementation and declaration have been removed from K's core, along with its allocator-specific test assertions under explicit user approval. Shared identity types remain available to K. Implement allocation and its exhaustion checks in the orchestrator, and separate shared protocol identity definitions from K's execution utilities as those interfaces develop. Orchestrator allocation is still pending; removing it from K does not complete that work. Further changes to existing test cases still require user approval.

The following decomposition implements the recorded contracts under Q03–Q17/Q23. Component boundaries and the transition-function API remain implementation proposals, not mandatory files or retained legacy functions. Q14 fixes the task model described below.

| Component | Contract and replacement boundary |
|---|---|
| Request and contract decoder | Bounded parsing of Q09's single supported schema version, logical authority checks, Q08 duplicate-submission discard without body comparison, static feature/limit checks, descriptor/P validation and safe arithmetic. New schema and selected codec independent of Beacon framing; reuse a codec library only after measuring its bounds. |
| Operation engine | Per-op phase, immutable request reference, commit/dispatch facts, accepted observations, fixed scope, deadlines and results. Implement all six operations as specializations of M1–M7; the legacy monitor and its operation hierarchy may be replaced. |
| Admission and binding registry | Atomic local resource/result reservation, instance fences, complete incidence linked to O reservations, exclusive binding identity and safe internal handles. Under agreed Q04, O releases bookkeeping for definitely unsent requests or matching pre-work rejection/complete cleanup; unavailable outcomes retain possible incidence. Device resource release follows actual local closure independently of O acknowledgement. Q05's post-history-discard limitation remains explicit. Define new storage around these invariants rather than extending the old module registry by default. |
| Runtime service | Reserve instance/ports, instantiate inertly, validate declared entrypoints, perform guarded single dispatch/execution, emit truthful STARTED, stop execution/access and fully reclaim node-owned resources and release associations/reservations before CLEANED. Stopped-but-unreclaimed nodes remain cleanup obligations; Q06/Q07 management evidence is retained separately. The engine and ABI are selected against this contract; the existing container manager is optional. |
| Endpoint service | Distinct role lifecycles, inactive port installation, activation permissions, evidence and scoped terminal cleanup. Choose buffer/queue storage from channel contracts and lifetime requirements; replace the current lazy queue paths. |
| Identity and evidence service | Validate operation, request, creation, instance, endpoint role, owner and issued-action matches. Under the revised Q07 agreement, each owner emits cumulative object facts with no revision counter: equal repeats; strict subset is old; strict superset advances; incomparable histories conflict. Kc keeps the aggregate revision per `(opId, Kc)` for complete snapshot ordering and exact storage ACKs. Every accepted advance adds facts/history, retaining prior evidence and lifecycle/terminal-outcome guards. Reject conflicts without partial mutation, cleanup selection or reservation release. Schema 3 uses facts bytes for objects. At nine objects, eight object facts and six history facts bound aggregate advances at 78, fitting uint8; admission limits scope and checked increments prevent wrap. Keep facts/aggregate ordering state through continuing optional work, unresolved cleanup and retained reports. ACKs/queries never reset it. Reclaim only when local settled-work, Q06 retention and access lifetimes permit; no global packet order or permanent replay barrier is claimed. |
| Resource and lifetime service | Bounded pools/quotas, allocation ledger including partial work, outstanding access accounting, control/cleanup reserve, deterministic failure hooks and high-water measurements. |
| Deadline and scheduler service | Q15: injectable monotonic 64-bit timestamps with checked duration conversion/addition; four fixed allowance categories; timely acceptance requires now < deadline at the owner's decision boundary. Process expiry at equality even if the timer event is delayed. Bound retries by original allowances; preserve late closure evidence and fair progress. Numeric values, unit and timer layout remain profile deliverables. |
| Result service and management interface | Retained attributed results, queries by original opId, separate historic outcome/current channel status and bounded updates. Q07 requires Succeeded/FailedClean/Unresolved results to be interpretable without earlier progress reports, preserving required history and remaining obligations. Consumers may discard processed message copies after incorporating their required information; this does not require every intermediate update to be cumulative. Under Q06, match O's or the designated DB's storage ACK to the specific evidence; reclaim eligible local acknowledged copies first, then evict the oldest eligible unacknowledged Succeeded/FailedClean history under pressure. Preserve queryable live state and operational obligations, including optional work after success. Defer admissions requiring capacity that cannot be reclaimed without discarding protected state. Provisionally accepted query contract: explicit no-retained-result response when local history is unavailable, with current observations/obligations distinguished; O may retrieve attributed reports from the DB. Rebuilding O's observed system view after state loss is future work, distinct from restoring management authority. A new simulator O client may use Python; operation conformance decisions remain in actual KaOS code. |
| Platform ports and build | Narrow clock, scheduling, transport, memory and runtime integration interfaces with host and embedded implementations. Build the same core directly for each; observation hooks must not change its protocol semantics. |

Q09 API/manifest agreement: use one shared integer `schemaVersion`, initially 1 and now 3, with exactly one supported version per build. Wire messages and standalone manifests identify their version; embedded manifests may inherit the envelope version. Accepted syntax/meaning changes bump the version; configured limit tuning does not. Define typed variants for the six graph operations, with separate query/report/storage-ACK message kinds. Share static supported-feature and finite-limit profiles between firmware configuration, O and simulator fixtures. O prechecks configured profiles; devices enforce actual local support and admission. Reject incompatible fresh requests before managed allocation or potentially allocating dispatch; a remote rejection after work begins retains cleanup obligations. Minimal envelope checks precede Q08 duplicate discard, and malformed requests never cancel an existing operation. Keep these features as simple as possible for the research prototype: static configuration and direct bounded checks, without negotiation, runtime discovery, downgrade, legacy adapters or a general compatibility framework. Coordinated schema upgrades and manual profile consistency are accepted limitations; profiles do not reserve current capacity. Exact fields/tags and numeric limits remain deliverables, with feature vocabulary informed by Q10/Q11/Q12/Q18.

Q10 predicate agreement: use only declared channel references, `true`, `false`, `and` and `or`, with simple bounded parsing/evaluation. Reject `atLeast`, `percent`, negation and executable predicates; no named threshold-group implementation, automatic expansion or expression optimizer is required. Validate references and require `P(Channels)` to be true before admission. Keep the same immutable P for preparation, startup and feasibility checks over the corresponding complete/possible channel sets. Repeated references/reports do not create additional qualifying channels. Bound expression size, nesting and declared channel count in the static profile; qualify concrete limits under Q01. Large threshold requirements may exceed practical expression bounds and are outside this baseline.

Q11 planning agreement: expose a minimal guest `put`/`get` data API, with concrete bindings managed inside KaOS. The manifest's channel type selects service behavior; Q18 selects one best-effort push service, with scheduled delivery deferred. Guest binding enumeration/selection is not required. Disabled calls return errors and never wait for activation; Q18 selects nonblocking calls with EMPTY/FULL results; Q13 selects bounded copies with no guest pointer retained after return. The bounded bidirectional event queue has three agreed placeholders from the 3 October Q11 revisit: KaOS-to-container channel-state-change notifications, container-to-KaOS generic `ERROR` without a channel field, and empty `INIT` from the first main entry for startup confirmation. Exact event payloads/signatures remain future interface work; no error-code vocabulary is fixed. KaOS supplies source attribution. Notification does not transfer binding management to the application or introduce an application-ACK gate, and local channel permission does not establish remote health. Overflow policy is agreed: INIT uses reserved startup/control capacity; channel-state notifications retain the latest pending state per channel, coalescing undelivered changes; ERROR uses a bounded FIFO and rejects new submissions with FULL when full, without blocking or overwriting accepted events. Coalescing does not preserve every transition or establish uninterrupted availability. Charge pending notification storage at admission, including unread notifications after channel removal, while releasing safe endpoint resources independently of consumption. Validate lifecycle/identity before updating pending state. Event reception is agreed: the container explicitly receives queued notifications at chosen main-loop points, checking immediately or waiting with a finite timeout. A wait yields only its runtime task, holds no control/registry lock and is interrupted by node cancellation/removal. KaOS does not asynchronously call guest event handlers; Q23 runtime interruption for cancellation still applies. A nonreceiving guest may leave notifications unread; permission checks and cleanup remain independent. For accepted ERROR events, the agreed placeholder policy is to report the error only, preserving container attribution through bounded diagnostic/reporting mechanisms. The event alone triggers no automatic cleanup, restart, graph change or operation-outcome change; independently detected runtime/protocol failures still follow their applicable rules. Proper error-response behavior is deferred to a later extension. Exact interfaces, reporting integration and numeric limits remain implementation work; no additional guest payload fields or delivery/storage guarantee are implied. Stable logical channel addressing is agreed, preserving the directed `(sourceLogicalNodeId, destinationLogicalNodeId, serviceId)` channel identity. A compact guest-local alias may encode that declared channel; delivery type is separate from identity and grants no implicit fan-out. Keep `(channelId, creatingOpId)` internally for the concrete binding; one binding per channel and complete removal before re-addition remain required. An admitted access, queued payload or callback cannot be redirected to a replacement binding. Preserve Put as local buffer acceptance, Get as application retrieval, and Send/Receive as internal endpoint transfer actions; Put success does not prove remote delivery. Keep guest-visible concrete binding handles out of the API. Q13 selects copying and Q18 selects nonblocking calls for the initial implementation.

Q12 lifecycle agreement: instantiate the runtime during preparation, before commit; the user's intervening after-commit suggestion was superseded. Stage configuration then, while deferring remaining guest function-call setup and guest-applied configuration until startup authorization. Preserve independently installed endpoint mappings and revalidate current bindings/permissions at execution and data access. Treat staged versus guest-applied configuration explicitly when refining M3 integration; instantiation alone does not prove later guest configuration or execution will succeed. Q04's parallel local/remote preparation is explicitly confirmed: remote preparation need not wait for local instantiation. Commit still requires successful instantiation and P(PreparedSet). Speculative remote preparation is accepted; local instantiation failure requires whole-scope cleanup including dispatched remote work. No additional preparation barrier or acknowledgement phase is introduced. Q12 selects the empty `INIT` event, named in the Q11 revisit, submitted as the first action on the first main entry after required guest configuration succeeds. KaOS arms acceptance for the authorized main invocation, attaches instance/operation identity, rejects setup-time/duplicate/stale/post-cancellation markers and preserves the normal success gates. The user accepts reliance on trusted workloads following this convention; a signal alone cannot identify the calling guest function, and a missing marker never proves nonexecution. Queue delivery/storage must be bounded and checked; a pre-call RUNNING flag is insufficient. This reuses the existing WAMR guest-to-host signal path and requires no interpreter entry-hook patch. Independent entry instrumentation remains optional. The revised integration still needs host/ESP32 qualification. Q23 resolves the inert-instantiation approach: qualify a restricted rebuilt artifact profile that rejects implicit start/constructor paths, with required initialization explicit after authorization. It also selects suppression of queued entry or runtime interruption followed by confirmed stopping and full reclamation. Pin and qualify the concrete WAMR backend/API on both targets; the conservative normal-return/trap policy below is agreed, with its integration and qualification tracked by R02.

Q12 exit policy: the baseline uses long-lived service nodes. Successful guest configuration returns into the authorized main-entry path; a configuration failure/trap or main return/trap before startup success is accepted makes the runtime unusable and triggers the attempted addition's already-authorized whole-scope cleanup of N and every intended endpoint, with StartIssued effects accounted for. No fresh O request is needed. Preserve original deadlines: FailedClean requires complete closure; otherwise outstanding obligations yield Unresolved at cutoff. INIT emission/queueing is not success acceptance. Serialize exit/usability updates with acceptance so an earlier marker cannot establish success for an already-exited, unusable instance. After accepted success, report normal exit or fault distinctly, preserve historical Succeeded, mark the instance locally unusable and disable new local endpoint data activity. No automatic restart or main re-entry. This direct runtime observation is separate from Q11's reporting-only guest ERROR placeholder.

After successful startup, reclaim safe local resources without waiting for O once accesses have stopped, preserving exact identities, fences, outstanding obligations/evidence and associations still needed by active work or later removal. Local endpoint users can still delay node-registry release. Charge unreclaimed resources; runtime exit or partial local reclamation is not CLEANED(N) or full distributed closure. Already-authorized channel operations keep their failure/cleanup obligations and deadlines. Full distributed node removal requires a fresh O-authorized request/opId for the exact instance, using Q04's complete current incidence (including later-added and in-flight authorized channels) and atomic authorization fence. This adds no acknowledgement phase to the original addition. O unavailability may delay that distributed removal indefinitely while safe local reclamation and admitted cleanup continue; an exit report alone neither clears O's incidence bookkeeping nor proves remote closure. No removal outcome/deadline exists until a removal is admitted.

Transport framing agreed during Q08: standard CoAP over UDP through a small adapter carrying KaOS control/report payloads, retaining Q07's reliable-datagram model with possible out-of-order delivery. Prefer standard tools and qualify the selected library's suitability and footprint on the Q01 ESP32 profile. The adapter maps native KaOS commands to valid CoAP requests and responses; POST to a control endpoint is illustrative, and exact paths/response mapping remain schema work. Preserve KaOS identities and the distinct meanings of receipt, admission, operation outcome and exact evidence-storage confirmation. CoAP methods/responses apply inside the adapter; the core need not expose a REST resource for each graph object. Concrete schema/profile, numerical limits and library selection remain deliverables. Q01/Q15 must account for bounded outstanding exchanges, retransmission/duplicate state and retry timing, and simulator impairment must reach the adapter's traffic. Revisit custom CoAP-derived framing only if measured footprint or a concrete integration constraint warrants it. Existing Q05 replay limitations and Q06 retention obligations remain explicit; no additional operation-completion phase is introduced.

Q08 payload/transfer agreement: use bounded deterministic CBOR for structured messages/metadata and configurable-length raw artifacts through standard block-wise transfer. Devices pull artifacts from O with sequential Block2 exchanges after admission; initially permit one bulk transfer per peer with independent capacity/service for control and cleanup. Freeze one report representation/revision during transfer and validate the complete body before accepting its evidence or issuing a stored-evidence ACK. Use standard transfer correlation to exclude mixed revisions/attempts. Size blocks within the complete-datagram budget without relying on IP fragmentation. Restart interrupted transfers from the beginning only within the original allowance; retain partial-work cleanup obligations under Q16. Keep logical-body/artifact size, block/datagram size, concurrent transfers and staging capacity independently bounded, and measure library reassembly and runtime binary ownership. Detailed schema/profile and storage placement remain implementation work.

Q23 qualification agreement: keep WAMR as the first candidate and qualify a small lifecycle prototype on host and Q01 ESP32 before considering another engine. Record each runtime revision, backend/build flags and guest artifact/toolchain profile. Require inert pre-commit instantiation through a restricted rebuilt artifact profile, explicit required initialization after authorization, and Q12's checked startup marker. Demonstrate cancellation of compute-only infinite loops, configuration and host imports; suppress queued entry when cancellation wins. Host calls must finish in bounded time or support cancellation. A termination request alone does not establish the stopped state: observe runtime-call completion and drain tracked accesses before resource destruction. Verify repeated lifecycle/partial-failure cycles, full node-resource reclamation, and peak ESP32 memory/stack use, accounting separately for endpoint and retained-management resources. Failure to stop/reclaim by cutoff remains Unresolved. Engine/integration changes are considered if a concrete contract or footprint failure warrants them; no qualification result is claimed yet. Q12's conservative normal-return/trap policy is agreed; qualify exit/acceptance races and safe local reclamation independently of O availability.

Q08 security scope decision: authentication is plan-only, with no DTLS/key-provisioning or cryptographic peer-authentication implementation required for the current research baseline. Use unsecured CoAP/UDP and preserve logical configured-peer/owner/operation/issued-work checks; message authenticity is an evaluation assumption, not a guaranteed property of those checks. DTLS with pairwise PSKs remains a future candidate, and possible Byzantine-fault-tolerance work requires a separate threat/model design. Do not claim protection against spoofing, tampering or malicious participants. Limit current integrity implementation to accidental-error detection/recovery; cryptographic artifact verification remains plan-only. Declared length, bounds, complete assembly and safe parsing remain required. Declared length plus CRC32 over the complete staged artifact is now agreed for accidental-error detection, alongside block completeness and ordinary transport checks; rejection/retransfer stays within existing deadlines and cleanup rules. No cryptographic hash is an implementation commitment here.

Q08 duplicate-submission agreement: O prevents conflicting opIds and resends its recorded immutable request; changed intent needs a fresh ID. Devices discard repeated top-level submissions identified by retained opId state without comparing/hashing their bodies, repeating managed work, resetting deadlines, replacing the original request/outcome or regenerating a graph-operation result because of the duplicate. Retain only state required for operation/identity/lifecycle obligations, not extra bodies/fingerprints solely for comparison. Preserve standard CoAP exchange ACK/response behavior and explicit status queries; phase/object/action messages, evidence, storage ACKs and idempotent owner cleanup still use their existing handlers. This is not a device-wide seen-opId filter over every protocol message. Document the deliberate M1 deviation: conflicting duplicate bodies are not detected/rejected, and identical duplicates do not trigger a new application result. After relevant history is discarded, Q05's replay limitation remains.

Architectural addition requested during Q04: include a DB/log collector role accepting system logs and attributed protocol reports for retrieval. The user agreed that its acknowledgement of recording exact evidence can replace O's ACK for local report retention. This can retain history beyond device-local reclamation and support future reconstruction of O's view. O must retrieve and validate the underlying evidence before changing its own bookkeeping; DB receipt is not O's observation of an outcome. Database engine, placement and whether this is a guest node or a separate service are open. Neither persistence nor survival of O/DB failure follows from the role alone.

Agreed DB contract: retain accepted protocol evidence until O acknowledges recording it. Decline archival writes when the DB cannot reserve space for that promise; devices fall back to the existing Q06 local policy. Export uses bounded resources independently of admitted operation progress and preserves evidence source/identity/revision; concrete buffering and delivery mechanisms remain design work. Diagnostic logs may use a separate retention policy. Archiving does not release local pending/unresolved obligations. Storage medium and failure guarantees remain open, so no restart durability or replication is implied by an ACK. The user's later extension is a pool from devices' spare capacity; donation/reclamation, placement/replication, discovery and storage costs require a separate design. No mandatory pooled-storage implementation or O recovery guarantee is added to the six-operation baseline.

A proposed bounded record model separates `OperationRecord` (request, scope, history, evidence and deadlines), `InstanceRecord` (ports, capabilities, runtime handle and access lifetime), `BindingRecord` (channel and creation identities, placements, endpoint roles and observations), resource reservations and retained results. Compact rejection records are optional under Q05, not a prerequisite or a permanent-retention commitment. These are logical ownership boundaries; share immutable descriptors or compact fields where it reduces measured memory without losing evidence. Capacity for continuing optional work must remain owned after the parent operation completes. Q03 fixes protocol IDs at 64 bits; Q07 retains only aggregate revisions, bounded by the supported facts and scope. Select the remaining layouts using measured Q01 costs and the agreed Q06/Q07 lifetimes.

Q13 ownership agreement: follow the [concrete resource table](graph_operation_design_questions.md#q13). Each allocation/reservation, including partial initialization, has one cleanup owner. Transfer staging hands retained artifact ownership explicitly to the node record; node/runtime resources and endpoint/channel resources outlive the operation that created them. Shared local-channel buffers have one owner and tracked endpoint users. Operation records own their work/timers; retained outcomes belong to the separate reporting store. Preserve ownership for continuing optional work. Under short local synchronization, validate identity/permission and register any access that will outlive the lock; cleanup blocks new accesses and waits for issued work to stop. Queued work carries concrete identities; direct retained pointers require tracked access, and late results must reclaim their private allocations without republishing removed objects. Put/get initially copy between validated guest memory and KaOS-owned buffers, retaining no guest pointer after return. Blocking data calls are deferred under Q18; a future blocking service requires tracked pending accesses and safe wake/cancellation. Full reclamation of allocations/associations/reservations precedes CLEANED; a zero access count alone is insufficient. Local release need not await remote evidence once safe, while whole-operation completion still requires every scoped object's closure. Concrete counters, locks, allocator layout and bounds remain Q01/Q14 deliverables.

Q14 execution agreement: use an event-driven system with one control task per KaOS, one runtime task per admitted node, and a small bounded service-worker set for endpoint/transport/cleanup work. Control handlers perform short state transitions driven by requests, completions, container events and timers; idle waits yield/block instead of busy polling. Guest execution, instantiation, network waits and reclamation waits stay outside the control task. Node and endpoint preparation remain parallel. Reserve capacity for required completion/cleanup evidence, including `INIT` startup evidence, and service due timers/cleanup between bounded batches of ordinary events. Pending cleanup cannot occupy the only worker needed for unrelated progress. Q13 access admission and lifecycle transitions use short synchronization; no lock spans guest execution or remote waits. Qualify stack/worker counts, queue bounds and scheduling budgets on ESP32. The final state/transition configuration must undergo TLA+ safety and liveness checking; see the verification strategy and Q14 for scope and assumptions.

Q15 timing agreement: use node-addition, channel-addition, cleanup/removal-evidence and O-response allowances with the M7 owners/start events. Measure numeric profile defaults and margins from intended workload transfer, instantiation, handler/queue and cleanup costs. No additional per-phase budget is required. Validate finite durations, conversion/addition without overflow, and channel deadlines no earlier than the parent node deadline. Optional channels retain original allowances after node success. At the owner's serialized acceptance decision, require `now < deadline`; at equality or later, expiry wins regardless of queue-arrival timestamps or delayed timer callbacks. At cleanup cutoff, record missing closure before incorporating late CLEANED as later evidence/resolution. Retries, queries, commit, phase changes and artifact restarts never reset original deadlines. Use bounded CoAP exchange retransmissions capped by the applicable allowance, and bounded configured O status-query intervals; duplicate submissions are not a result-replay mechanism. Stop a cleanup attempt's retransmissions at cutoff; additional attempts require authorized reconciliation, and scope expansion does not renew exhausted channel-cleanup windows. Local safe cleanup and acceptance of late evidence may continue. Ki never interprets Kc's clock as its own, and expiry proves neither stopped remote execution nor reclamation. Include these boundary/retry rules in Q14's final TLA+ checks and deterministic tests.

Q16 staging/error-path agreement: keep bounded pre-admission decoder workspace separate from admitted artifact-transfer resources. Reject invalid requests before graph work and release temporary workspace/provisional reservations. Before artifact pull, reserve declared bounded staging capacity, measured peak runtime/staging overlap and completion/cleanup capacity; this does not require whole-artifact RAM storage. The transfer record owns partial bytes, transfer state and outstanding callbacks until full declared-length/CRC32 validation and explicit handoff to the node/loading owner. Preserve backing storage while WAMR needs it. On a permitted restart, stop the old attempt, discard partial contents and reuse reserved storage safely within the original allowance; old blocks cannot repopulate reused storage. Permanent artifact failure, exhausted node-addition allowance or instantiation failure selects whole-addition cleanup, including all intended endpoints and remote work already dispatched. An individual endpoint failure retains the existing channel/predicate consequences. Never report allocation-free rejection after managed work begins. Reclaim every partial allocation under its owner; unreclaimed staging or callbacks remain charged and prevent CLEANED. FailedClean requires full scoped closure; missing closure at cutoff is Unresolved. Concrete staging medium and WAMR-retained binary lifetime still require qualification.

Q18 channel agreement: initially implement one manifest-selected best-effort push service with bounded message/byte capacities and maximum payload size. Nonblocking put copies a complete valid message into outgoing storage or returns FULL without accepting/overwriting it. Nonblocking get copies and consumes the oldest incoming queued message, or returns EMPTY; invalid/undersized guest buffers do not silently truncate or consume the message. Disabled access returns an error. On receiver overflow, drop the incoming message while retaining queued messages. Transfer is asynchronous; put success means local acceptance only. Local FIFO does not promise network ordering, delivery, deduplication or freshness. Add no channel-service retransmissions, remote acquisition on get, automatic expiry or per-channel delivery scheduler. Applications choose when to put and handle ordinary results and message usefulness; FULL/EMPTY/loss alone do not fail graph operations. Preserve the same contract for local/remote channels, exact binding identity and full cleanup of queued data under Q13. Q08 management reliability is separate. Scheduled, pull, reliable, overwrite/latest-value and blocking services remain future work. Measure numeric limits under Q01 and model/test selected behavior under Q14.

Q19 clock/freshness agreement: defer distributed clock synchronization for the initial best-effort service. Retain Q15 owner-local monotonic deadlines. Applications may measure local time since a precisely defined reception/read event; this does not bound acquisition age, and recent arrivals may contain delayed/duplicate old data. Add no mandatory timestamp field or new guest API. If experiments carry acquisition timestamps/sequence numbers, specify clock, units, source instance and event; do not subtract unrelated device clocks or treat sequence numbers as elapsed-age evidence. Simulator observer timestamps may measure acquisition-to-delivery latency only where acquisition is actually observed/defined; keep that oracle out of guest/protocol decisions and inject device skew/drift to expose shared-host assumptions. A future workload requiring maximum acquisition age must select and qualify clock mapping/synchronization, uncertainty and provenance, then define stale-data handling. Q20 owns local observation-gap thresholds/reactions. No freshness guarantee or Q18 expiry policy is introduced.

Q20 partial agreement: the minimum ventilation control path requires usable CO2 sensing and functioning local airflow feedback/control/actuation; occupancy may be absent in degraded operation. Evaluate service targets using a defined Quality of Control (QoC) metric, with nominal/degraded acceptance criteria separate from graph-operation success. The controlled quantity/reference, formula/units, evaluation window, thresholds and treatment of missing observations remain to be selected with the Q21 model. Do not infer QoC from liveness or successful transport. Timing, validity checks, fallback action/duration and escalation remain open; no physical fallback is selected by the minimum-path agreement.


Q21 model agreement: use a low-fidelity host-side, well-mixed single-room CO2 balance with bounded actuator dynamics and simulated CO2/occupancy/airflow peripherals. Run validation, policy and local feedback control as actual KaOS/WASM workloads, preserving peripheral placement and keeping plant state independent of guest lifetime. Qualify units, simple reference cases, integration-step and parameter sensitivity before QoC claims; omit spatial gradients, multi-room and detailed HVAC physics initially. Keep ESP32 resource/timing qualification separate. The concrete model has two continuous states, room CO2 concentration and mechanical outdoor airflow: fixed room volume, scripted occupant count and per-person CO2 generation, constant outdoor CO2 and optional constant background exchange, and a bounded actuator command mapped to airflow with a single response lag. A powered actuator retains its last command during communication/guest failure; explicit actuator power loss drives target airflow to zero, with actual flow decaying according to the lag. Sensor failures affect observations without changing plant truth. Exclude thermal/humidity, energy-consumption, weather/pressure and detailed duct models; these exclusions bound evaluation claims. This plant behavior does not select an application fallback. Measurement tooling/coverage remains under discussion, and Q20 metric/threshold/fallback choices remain open.

Use the same message acceptance functions for local events and decoded remote messages. Do not enforce ordering only in the simulator. Keep a short consistent decision boundary around checking evidence/deadlines and recording commit/start/failure. Release locks before guest execution, network waits, allocation that can block, and user callbacks. Reserve any required completion-event capacity before issuing work so the implementation cannot lose the only completion or cleanup notification.

Q17 management agreement: cancel a potentially dispatched addition through the appropriate node/edge removal with a fresh opId and exact authorized target/scope. A separately authorized retry after Unresolved is another removal request with its own opId/allowance; prior-attempt links remain only in O's records, with no predecessor field in device requests/reports. KaOS shares owner cleanup by exact instance or `(channelId, creatingOpId)` and keeps bounded per-request scope/deadline/result records, freeing each concrete object once. A new attempt never resets an older deadline; valid late closure may resolve retained Unresolved outcomes, while historical Succeeded/FailedClean remain unchanged. Reject/defer a new observer before acceptance if result/observer capacity is unavailable, without stopping existing cleanup. Queries use the original opId and transport correlation, return retained evidence or no-retained-result, and authorize no new work. Apply Q07 result revisions and Q08 duplicate-submission discard; no query/retry reconstructs an evicted historical result. Keep Q04's unsent-withdrawal rule and full-incidence scope checks; a dispatched removal cannot be withdrawn to resume construction. Device wire fields, errors and record layout remain implementation work.

For each object, keep its creating authorization distinct from a later removal request. A shared owner cleanup task may fulfill multiple observers' obligations, but each accepted reply must establish closure of the exact object for the receiving request. Registry release may use a conservative endpoint-release-first order; it must not require a remote endpoint's memory to vanish before safe *local* storage can be freed. Full operation completion still requires all remote evidence.

## Embedded memory and processing plan

Use finite provisional Q01 limits from the first prototype, then qualify them before fixed layouts and supported-capacity claims. Define separate bounds for `N` resident instances, `E` local endpoint roles, `O` in-flight operations, `R` retained full results, `Q` queued events, `B` queued payload bytes, artifact staging and expression complexity. If compact rejection storage is implemented, bound it separately as `T`; Q05 does not require it. A node-addition cleanup scope contains `1 + 2C` logical objects for C declared channels even if only some are local. A node's current incident scope may be larger than its original manifest and must have its own admission bound. Also bound shared-cleanup observers, revision updates, CoAP exchanges, bulk transfers and export buffers; reserve closure/report capacity for every admitted scope.

Build a measured memory ledger:

```text
peak managed RAM = fixed platform/control/runtime baseline
                 + instance records and actual per-instance WASM/native allocations
                 + endpoint records and uniquely owned buffers
                 + in-flight operation and full retained-result records
                 + evidence indexes and any optional compact rejection records
                 + event/retry queues and artifact/decoder staging
                 + temporary overlap during preparation and cleanup
                 + reserved control/cleanup capacity and measured headroom
```

Count shared buffers once, while retaining separate endpoint obligations. Include allocator overhead, alignment, fragmentation, WASM linear memory, binary ownership/copies, thread-local storage/native stacks, network buffers, timer structures, and result serialization workspace. Q08 adds no transport-security stack to the baseline. Host `sizeof` and RSS are not 32-bit target RAM measurements. A detailed report may be much larger than its control record; bound both storage and response serialization. If persistence is selected later, budget flash layout, write amplification, wear and torn-write recovery separately.

Admission must reserve peak overlapping needs, not just the final steady state. Charge partial/failed/unresolved allocations until safely released; keep result slots charged after work capacity is freed. Optional channels must not exhaust resources required for a P-satisfying support set indefinitely (P3). Consider quotas or reservation priorities, subject to Q04/Q14; fixed unlimited fan-out is not acceptable.

Use preallocated pools or bounded allocators where measurements justify them; audit allocation in the selected WASM engine, codec and network paths before promising allocation-free operation. The implementation can eliminate duplicated legacy registries, compatibility layers, artifact copies and unnecessary native stacks. Compare these savings against the real cost of required evidence/lifetime state and any optional rejection metadata; preserve the selected scope and Q05 exception. Avoid one native stack/timer per channel where bounded shared scheduling can meet progress. Represent qualifying sets as bounded bitsets and P as a bounded expression; evaluate it in work proportional to its encoded size. Do not enumerate every subset on the device to validate a positive Boolean grammar.

Measure event-handler time, lock hold time, startup/cleanup latency, timer lateness, queue occupancy, memory peaks and retained bytes during outage/churn. Derive deadline allowances from preparation, delivery/retry, dispatch/entry/confirmation, scheduling and handler bounds with stated headroom. Eventual fairness is insufficient for numerical deadlines. Test `limit−1`, `limit`, and `limit+1` at every resource boundary and prove that resource exhaustion still leaves enough control capacity to report or clean admitted work.

## Conflicts and reconciliation

The first six entries reconcile existing wording or explicitly accepted differences; they do not request new decisions. The last three identify boundaries that must be resolved or qualified before the affected implementation is fixed. Detailed discussion history and reference sources remain unchanged.

| ID | Conflict or tension | Treatment in this implementation plan |
|---|---|---|
| CF01 | The old P0 package reopened Q03–Q09 and described runtime/ownership/scheduling contracts as unselected. | Use the recorded answers. P0 now produces concrete schemas and qualification evidence for the selected baseline. Numerical limits, field tags and library choices are implementation deliverables, not repeated architecture questions. |
| CF02 | [Q11](graph_operation_design_questions.md#q11) previously retained wording that nonblocking-only calls were not accepted despite [Q18](graph_operation_design_questions.md#q18); the 3 October Q11 revisit corrects that stale sentence. [Principle 12](graph_operation_design_principles.md#12-extend-a-concrete-baseline-without-assuming-future-guarantees) still describes standard versus derived CoAP framing as unselected because it summarizes Q01–Q07. | Q18 supplies the current call contract; Q08 supplies standard unsecured CoAP/UDP and deterministic CBOR. The earlier transport discussion remains review history, not a current alternative. Q11 now settles event overflow separately: reserved INIT capacity, coalesced latest channel state and a bounded ERROR FIFO returning FULL. Q11 also agrees explicit event reception with an immediate check or cancellable finite wait and reporting-only ERROR handling as a placeholder; proper error-response behavior is deferred to an extension. Data FIFO overflow and nonblocking put/get remain settled under Q18. |
| CF03 | SH M1 requires changed-body duplicate rejection and retained-result replay; Q08 selects duplicate-submission discard without comparison. | Enforce immutable request association at O. Retained-opId submissions have no repeated graph effect or duplicate-triggered result. Keep valid CoAP exchange behavior, explicit queries and phase/object handlers. Record this as a deliberate reference deviation, not full M1 conformance. |
| CF04 | SH M6 and reference models preserve obsolete-construction rejection after retirement; Q05 accepts losing that protection. Earlier memory/record wording could imply mandatory compact rejection storage. | Keep live/pending identity, lifetime and cleanup checks; make compact rejection storage optional. Characterize post-history-discard resurrection and its effects. Neither unique IDs, revisions nor successful cleanup repairs the accepted gap; do not require a new prevention mechanism to start implementation. |
| CF05 | Reference evidence assumes authenticated origins; Q08 implements no cryptographic authentication or artifact verification. | Keep authentic, non-Byzantine participants as an evaluation assumption; use length/CRC32 for accidental corruption only. Logical identity checks remain mandatory. Security mechanisms remain future work. |
| CF06 | Device pressure eviction under Q06 might be applied to the DB, or a storage ACK interpreted as cleanup/O knowledge. | Device history can be evicted only under its eligibility rules. The functioning DB retains each accepted evidence item until O acknowledges it, refusing writes it cannot retain. No archive supersession/compaction is selected. ACKs reclaim reporting copies, not operational obligations; O releases its bookkeeping only after validating appropriate evidence. |
| CF07 | SH M3 says instantiated with required configuration; Q12 stages configuration before commit but applies guest configuration after startup authorization. WAMR instantiation can also execute implicit guest initialization. | Preserve the agreed ordering and document the staged/applied distinction in the concrete runtime contract/model. Q23 selects restricted rebuilt artifacts with no pre-commit guest execution; required initialization runs explicitly after authorization. Qualify both backends. Do not silently claim that ordinary WAMR instantiation already satisfies this contract. The conservative normal-return/trap policy is now agreed; R02 tracks its concrete integration and qualification. |
| CF08 | The aggregate counter must fit the final schema and maximum scope, including optional work and late cleanup. | The selected P2 profile bounds advances at `8 * objectCount + 6`, at most 78, fitting an 8-bit aggregate counter. Objects carry facts only. Revalidate this bound before changing report facts or scope limits; widen the aggregate or reduce scope if needed. Cleanup reporting remains reserved. Physical capacity qualification is separate. |
| CF09 | Q18 has no expiry/order guarantee and Q19 defers clock synchronization; Q20 validity/QoC/fallback is open. Q21's actuator holds its last command during guest failure. | A recent receive is not proof of acquisition freshness, and plant hold behavior is not an approved application fallback. If the eventual workload requires cross-device maximum age, specify a qualified clock/provenance extension. Select QoC claims within the agreed plant's observable CO2/airflow behavior, or explicitly revisit its scope. |

## Remaining information and implementation gates

The R01–R08 register below retains the implementation gates and their agreed scope. Completed implementations and current evidence are summarized in [current status](#current-status-and-verification-baseline); the [current decisions](#current-decisions-and-dependencies) and [agreed work](#agreed-work-that-can-proceed) below identify what remains. Unanswered application decisions do not block unrelated qualification. Provisional fixture settings must be labeled and finite; each package must close its relevant gates before freezing dependent behavior.

| ID | Information still required | How it is supplied and what it gates |
|---|---|---|
| R01 | **Guest/event ABI and qualification (Q11/Q14).** Channel-state change, generic `ERROR` without a channel field, and empty first-main-entry `INIT` are agreed placeholders. Reserved INIT capacity, latest-pending-state coalescing per channel and a bounded nonblocking ERROR FIFO returning FULL are agreed. Explicit event reception with an immediate check or cancellable finite wait is also agreed. Accepted ERROR handling is reporting-only under the agreed placeholder policy; proper response behavior belongs to a later extension. Specific payloads/signatures, reporting integration and numeric limits remain implementation deliverables. Specify put/get arguments, sizes/errors and logical channel addressing for later dynamic additions. | Q11's behavioral baseline is settled. Specify and qualify the concrete interfaces/layouts before finalizing P3/P4 event handlers; this is implementation work, not a pending error-policy decision. Data-call nonblocking/FULL/EMPTY/drop-incoming behavior remains settled. Preserve INIT attribution/STARTED gates and reserved startup/control completion capacity; budget unread notifications without retaining reclaimed endpoint resources. Qualify event-wait cancellation and independent control/cleanup progress, with no asynchronous guest event callback. |
| R02 | **Exit integration and qualification (Q12/Q23).** Conservative policy agreed: configuration failure or main exit before startup success triggers existing whole-addition cleanup; after success preserve Succeeded, report exit/fault, disable local use and permit safe local reclamation. Full distributed node removal after success requires a fresh O-authorized operation with complete current incidence. No automatic restart. | Implementation deliverable for P3/P6, not an unanswered policy decision. Specify attributed exit reporting, serialized exit/usability versus INIT acceptance, retained obligations during local reclamation and subsequent removal of an already-exited instance. Qualify LIFE11–LIFE13 on host/ESP32, including O unavailability, active channel operations and full resource accounting. |
| R03 | **Evidence and management schema (Q04/Q06–Q09/Q17).** P2 schema 3 selects complete cumulative owner facts and aggregate snapshots, aggregate-only revisions with a 78-advance bound, exact storage-ACK keys, query/missing-result distinction and bounded scope. | Preserve this contract through P3–P7 execution, issued-action/source guards, collector/ACK integration and shared cleanup. Keep multipart consistency and Q06 retention; a newer revision grants no DB supersession rights. Extensions to facts/scope require a new aggregate bound. |
| R04 | **Transport, artifact and data adapter details (Q08/Q09/Q13/Q16/Q18).** Select/pin CoAP and CBOR libraries; define paths, response/error mapping including discarded submissions, tags, CRC32 parameters, transfer correlation and staging medium. Specify the application-data carrier/frame, exact binding attribution and oversize handling separately from management CoAP. | Engineering selection and measured qualification in P0/P2; required for production control integration and P4 remote data. Share host/device fixtures. Q18 settles the application service, not its wire carrier; selecting one must preserve best-effort semantics. No additional user choice is required for routine tags, CRC parameters or library experiments. The [planned transport scheduling increment](#planned-transport-scheduling-increment) extends the current adapter with two sockets and bounded class service; preserve Q18 semantics. |
| R05 | **Qualified embedded profile and runtime (Q01/Q07/Q10/Q14/Q15/Q23).** Actual board/firmware/runtime/toolchain configurations; usable RAM/flash; resident/endpoint/operation/result/observer limits; payload/expression/transfer bounds; guest and native stacks; worker/event/control reserves; timing unit, four allowances and CoAP/query schedules. Demonstrate inert instantiation, startup attribution, noncooperative interruption and full reclamation. | Measured P0 prototypes, refined in P9. Board access and representative guest artifacts/traffic are required for physical measurements, not for initial host work. Two nodes/one local channel is the minimum, not a maximum. Pin both host and embedded runtime builds; qualify distributed placement separately. Gates supported profiles, fixed sizing and timing/feasibility claims. |
| R06 | **Collector realization (Q04/Q06/Q07), implemented in P7.** Bounded host RAM collector with explicit authorized-scope registration, a separate enrolled UDP socket and query/store/ACK handoff. Exact report versions remain until O ACKs each; no supersession. Retrieval/O ACK are trusted local APIs, diagnostics have a separate bounded eviction ring, and full storage refuses the copy before any device ACK. | [P7 contract](graph_p7_contract.md) records capacities, interfaces, backpressure and failure behavior. No device exporter task/buffer, persistence or replication is selected. Collector process loss may lose the only historical copy after device retirement; archive availability never gates operation completion. |
| R07 | **Ventilation control and service criteria (Q20).** Supervisory policies with both inputs and CO2 only; local controller law; sampling/control periods, plausible-value/rate checks, observation gaps and duplicate/reordered command handling. Supply QoC controlled quantity/reference, formula/units, evaluation window, nominal/degraded thresholds and missing-observation treatment. Supply fallback action, duration, expiry and escalation. | These workload decisions need explicit answers before implementing the final application behavior and service pass/fail evaluator in P8. Deterministic plumbing fixtures and graph-operation testing may proceed. Occupancy optionality is settled; no simulator default or actuator hold assumption fills these gaps. |
| R08 | **Plant parameters and measurement coverage (Q21).** Supply room/generation/background-flow/actuator parameters, initial state, occupancy script, sensor/noise rates, integration step and fault scenarios. Choose observations/tools/rates needed for each claim, including plant truth, actual guest consumption, commands/flow and protocol/resource events. | Experiment-design decisions and qualification before P8 QoC acceptance. The Python observer/JSONL/offline-metric route is a proposal, not an agreed tool choice. Check conservation/reference cases, physical bounds, step-size/parameter sensitivity and plant/controller/network time mapping. Report observation loss and simulation limits; keep evaluator truth unavailable to guests. |

Before release, P9 must also supply Q14's final TLA+/TLC safety and conditional-liveness evidence, with finite bounds, fairness/failure assumptions and handler-to-model traceability. This verification is mandatory but is work to perform, not information the user must provide before coding. Authentication, full stale-construction prevention, clock synchronization, reboot/takeover, pooled storage, migration and automatic recovery stay deferred; none is a hidden prerequisite for the agreed baseline.

### Current decisions and dependencies

| Area | Details still needed | What depends on it |
|---|---|---|
| Application-CoAP and traffic manifest | Required application rate likely belongs in the manifest, whose interface is still being designed; settle the CoAP resource/profile, response suppression, message-ID reuse, duplicate handling and congestion behavior | Protocol slice P and coordinated host/simulator wire migration; maximum supported remote rate is a qualification measurement, not a preselected rate cap |
| Ventilation control, R07 | Supervisory rules with CO2 plus occupancy and with CO2 alone; local airflow controller; sampling/control periods; plausible-value and rate checks; handling observation gaps and duplicate/reordered commands | Final control guests and application fault responses |
| Quality of Control, R07 | Controlled quantity and reference; formula and units; evaluation window; nominal/degraded thresholds; treatment of missing observations | APP05–APP10 service acceptance and healthy/faulted comparisons |
| Fallback, R07 | Trigger and response to unknown evidence; permitted action; maximum duration; expiry, restoration and escalation rules | Occupancy/CO2/actuator/supervisor-loss behavior; no universal hold-last or stop policy is selected |
| Plant experiment, R08 | Room volume, CO2 generation/background exchange, actuator range/lag, initial state, occupancy script, sensor/noise rates, integration step and fault schedules | Numerical plant experiments and their validation |
| Measurement coverage, R08 | Which observer supports each claim; actual guest consumption versus host copies; command versus physical response; sample rates, clock mapping and observation-loss treatment | Freshness, end-to-end timing and physical-service claims |
| Continuing-operation proposal | Concrete assessment/report interfaces; supported/violated/unknown validity rules; monitoring/refresh budgets; ordering and stale-evidence treatment; service-mode transition rules | Implementing the proposed runtime service-assessment layer |
| Recovery proposal | Feasibility and placement inputs, objective priorities, interruption and switching limits, restoration evidence, any delegated authority and conflict rules | Automated repair and optimization beyond explicit authorized graph operations |

The [application-CoAP plan](graph_core_simplification_plan.md#application-over-coap-separate-protocol-slice)
proposes wrapping the existing DataFrame in best-effort NON messages, retaining
the two traffic classes and bounded scheduling. Application datagrams still use
the existing DataFrame wire format today. Neither a manifest schema nor a rate
cap has been selected. Existing wire-fixture changes need specific approval;
the completed cleanup did not authorize the protocol migration.

The R07 and R08 entries above define the application and experiment gates.
The detailed source documents are
[application operation](</home/karolina/Research/Thesis/protocol specs/application_operation_specification.md>)
and [intent–condition scenarios](</home/karolina/Research/Thesis/protocol specs/application_operation_scenarios.md>).
The application document explicitly labels itself a proposal; its requirements
are not silently adopted as previously agreed implementation behavior.

The scenario profiles are useful for working out these choices. For example,
IC-04 needs source validity, selection and interruption rules in addition to
an OR startup predicate. IC-07 needs authority fencing and duplicate physical
effect handling beyond channel identity. Neither mechanism follows merely
from successful graph addition.

### Agreed work that can proceed

- Remaining protocol observation and fault experiments: named lifecycle
  barriers, complete observation-loss accounting, collector/crash scenarios,
  and repeatable replay checks. [Configuration and main-entry barriers](../verification/graph_core/barrier_verification.json)
  now cover removal and exact controller-deadline expiry with real WAMR workers,
  unrelated-worker progress and repeated partial-order checks. Preparation and
  disposal completion now also have manually scheduled adapter-contract checks:
  late preparation cannot revive startup, and cleanup cutoff retains ownership
  until disposal. [Real WAMR preparation/destruction gates](../verification/graph_core/wamr_phase_barrier_verification.json)
  now also hold actual instantiation and deinstantiation boundaries, check
  cancellation and cleanup cutoff, and require unrelated worker startup and
  cleanup during those holds. Preparation-failure injection and broader process
  replay remain open. The later worker-startup unwind test covers failed worker
  creation and joining already-created workers; it does not close WAMR
  instantiation/allocation-failure qualification.
  [Observation corruption checks](../verification/graph_core/observation_loss_verification.json)
  now make sample/timing assessments inconclusive for missing, duplicated or
  reordered records and nonzero reported drops, and reject truncated collector
  snapshots. [Live relay overflow](../verification/graph_core/relay_overflow_verification.json) now counts
  dropped packet copies and rejects normal run completion. A one-slot UDP test
  exercises actual overflow and distinguishes configured link loss. Kernel loss,
  default-capacity throughput and delayed-injector qualification remain open.
  [Collector process crash checks](../verification/graph_core/collector_crash_verification.json)
  now cover SIGKILL before/after the device accepts a storage ACK: a remaining
  device copy can be retrieved; lost archived history leaves O's possible
  incidence retained. [Shared-host crash checks](../verification/graph_core/host_crash_verification.json)
  now kill two active sink instances and their endpoint/coordinator roles together;
  surviving-source removal remains Unresolved and O retains both connections.
  Unrelated-channel isolation during a crash, freeze/resume and broader device
  crash schedules remain open.
  Existing finite fixtures cover selected paths.
  Absence of an observation cannot establish nonexecution or cleanup.
- Qualify the implemented [transport scheduler](../verification/graph_core/transport_scheduling_verification.json)
  on the target: sustained application/management load, timer lateness, worker
  progress, network buffering, stack/heap and lock timing. Two sockets, bounded
  rounds, recurring management service and artifact pacing now pass host checks;
  the relay retains one impairment policy per directed link and a global
  pending-copy limit shared by both classes.
  Short POSIX before/after experiments preserve feedback during artifact transfer.
  Checksum timing is recorded; optimisation remains conditional on declared
  workload targets and target measurements. The priority-3 control task can
  remain runnable ahead of priority-2 workers under sustained load.
- Final Q14 model work and TLC runs covering the composed implementation's
  queues, workers, timers, startup, channel operations, reporting and reclamation.
  The new HostExecution model joins several of these mechanisms in finite
  scenarios. Complete scheduler/collector/admission coverage and concrete
  trace mapping are still deliverables, not user-supplied policies.

APP01 acquisition scheduling and freshness are not established by the current
relative event waits or publication-to-GET measurements. OBS09 permits clock
experiments, but any acquisition-age claim needs an explicit provenance and
clock-error model. These boundaries must stay visible when adding fixtures.

### External qualification

P9/R05 requires physical memory/stack/timing measurements and distributed Wi-Fi
and collector checks on the declared target configuration. Host sanitizer
results cannot substitute for those measurements. Earlier qualification work
reported no serial board available; the cleanup made no new board-availability
check or physical measurement. Board access must be checked when scheduling
these experiments. Formal work does not depend on board access; the recorded
model runs used the locally installed TLC tool.

### Deliberately deferred scope

Restart, persistent recovery, takeover, migration/live replacement, in-place
rebinding, reliable application channels and autonomous recovery remain
extensions. Cryptographic authentication and complete rejection of obsolete
construction after forgotten history are also outside the selected baseline.
The accepted Q05/Q08 limitations remain qualifications of results.

The draft [design chapter](</home/karolina/Research/Thesis/kaos_design_v2.tex>)
lists migration and broader contract monitoring. Those outline entries do not
override the explicit Q22 deferral or select a monitoring interface.
## Implementation sequence and completion gates

These are dependency-ordered work packages, not calendar estimates. Each ends with reviewable code, its stated tests and an updated evidence inventory; no package is complete merely because a model previously passed. P0 outputs can unblock their consumers independently: a pending ventilation decision or board measurement does not block unrelated deterministic core work. Implement ownership, revisions, bounded reporting and cleanup with each vertical slice; P7 completes their management/archive integration rather than introducing those obligations late.

| Package | Work and dependency | Completion gate and principal tests |
|---|---|---|
| P0 Contract specification and feasibility prototypes | Apply the settled Q01–Q23 baseline. Draft R01–R04 contracts, produce finite provisional R05 profiles, qualify WAMR first on host/T-Display and CoAP/CBOR footprint, and record reuse/adapt/replace decisions. Apply the agreed Q11 event and Q12 conservative exit policies when specifying the R01/R02 interfaces and qualification cases. | Reviewable schemas/ownership contracts and prototype evidence with explicit pending R entries. Show inert instantiation, checked marker attribution, cancellation of compute loops/imports and reclamation; measure two-node/one-local-channel feasibility and peak overlap. No broad engine comparison unless a concrete failure warrants it. |
| P1 Shared core and deterministic harness | Establish the new core skeleton and minimal host/embedded builds. Add injectable monotonic clock, action barriers, allocation/access ledgers and attributed trace events. Model Q14's event-driven control/runtime/worker transitions in TLA+ alongside the core. Can begin alongside P0. | Same core builds through both ports; harness separates dispatch/execution/evidence acceptance and observes resource ownership. BASE/OBS tests target the new design; finite provisional bounds and model-action mapping are recorded. |
| P2 Contracts and admission | Implement schemaVersion 3, Q03 64-bit identities, Q08 duplicate discard, strict bounded CBOR and Boolean P validation, O's atomic authorization/incidence fence and each device's check/reserve/accept transaction. Use cumulative object facts without counters; reserve cleanup, aggregate reporting and completion capacity. Implement post-admission artifact pull with owned staging, length/CRC32 and failure handoff. Depends on P1 and relevant R03–R05 outputs. | REQ/PRED/CON/RES and BASE06/07: no managed work before admission, no over-admission or duplicate restart, valid CoAP exchanges, bounded complete-body validation and full cleanup after dispatched work fails. Establish aggregate revision-update budgets before enabling a profile; characterize Q05 separately. |
| P3 Runtime service and no-edge operations | Implement the qualified WAMR integration and R01/R02 ABI/exit policy, inert pre-commit instantiation, authorized configuration/main and attributed empty startup marker. Add no-edge node add/remove with single-owner resources, tracked accesses, strict deadlines and self-contained results. Depends on P2 and relevant R05 evidence. | HP01/HP02; F02/F09–F12; catalogue C03–C05; LIFE/RES and applicable TLC checks. Demonstrate marker/cancellation/exit races, pre-success cleanup, post-success safe local reclamation without O, bounded control progress and full reclamation under partial failure. Stopped-but-unreclaimed resources remain charged and Unresolved at cutoff. |
| P4 Endpoint service and edges | Implement independent endpoint preparation, receiver-first activation and scoped edge add/remove between established nodes. Add Q18 best-effort push, copied nonblocking put/get, bounded FIFO/FULL/EMPTY/drop-incoming behavior and exact binding checks through the R04 data adapter. Depends on P2/P3 and R01's channel/event ABI. | HP03–HP06; PERM/ID and catalogue C01–C06; no peer runtime destruction, no data before role permission, safe shared local buffers and local/remote semantic parity. Ordinary queue/loss outcomes do not fail graph operations. |
| P5 Multi-channel node addition | Add immutable P gates, independent work, PossibleSet, startup races and continuing optional-channel ownership after success. Depends on P3/P4. | HP07–HP12, PRED, F00–F15/C01–C06, optional deadline tests and all three failure histories. |
| P6 Full node removal and overlapping work | Compute/fix complete current scope, fence additions, cancel pending creators, preserve historic successes, aggregate every closure obligation, including fresh removal of an exited instance whose safe local resources were already reclaimed. Depends on P4/P5. | HP13–HP16, CON01–CON15, REM tests including pending optional work and distributed authorization in flight. |
| P7 Reporting, collector and reconciliation | Complete Q06 retention/queries, R06 collector export/retrieval/exact storage ACKs and retain-until-O-ACK behavior. Integrate Q17 fresh removal attempts with shared physical cleanup and bounded independent observers. Preserve revisions/obligations through optional work and late closure. Contracts start in P2; finish on P5/P6 and R03/R06. | MGMT/ID/RES, including MGMT17: direct/archive reordering, obsolete ACKs, DB-full refusal and DB/O loss, eligible device eviction, missing history, shared-cleanup observer exhaustion and no deadline reset. Archiving never gates operation completion. Record Q05 characterization separately from reference conformance. |
| P8 Simulator integration and applications | Integrate concurrent O, new executable, control/data impairment and protocol observations as P2–P7 land; run deterministic rebuilt workloads first. Separately implement the agreed Q21 plant/simulated peripherals and Q20 WASM control/evaluator once R07/R08 are supplied. Use Q17 removal/fresh-add scripts for explicit recovery experiments. | Protocol integration can pass independently of ventilation acceptance. APP/QoC gates require selected metric/policies/parameters and adequate observations, qualified model/time mapping and matching healthy/faulted scenarios. Keep plant state through guest removal and oracle truth out of guests. Unanswered criteria remain blocked, not passed. Include the [planned transport scheduling increment](#planned-transport-scheduling-increment), two-port runner/relay integration and contention checks. |
| P9 Final target and formal qualification | Refine R05 with functional packages: host sanitizers/allocation/churn tests, relevant embedded protocol cases, target memory/stack/scheduling measurements and distributed Wi-Fi placements. Run final Q14 TLA+/TLC safety/liveness configurations and map witnesses/counterexamples to runtime tests. | No-PSRAM T-Display meets the minimum workload and declared supported limits with control/cleanup headroom. Numeric allowances have measured support; final models include queues/workers/timers/access/reclamation and documented assumptions. Keep Q05/Q08 exceptions, Q12 trust and experiment limits explicit; no new run is claimed by this plan. Qualify the planned transport scheduler under concurrent application, management and artifact load, including checksum timing and the final Q14 service assumptions. |

Do not postpone cleanup until after the happy path: each allocation/start feature ships with its cancellation, partial-failure and retained-evidence behavior. Similarly, enable new concurrent simulator operations only with stable protocol correlation, not with the existing globally serialized host command sequence as a substitute.

Q22 scope agreement: deliver and evaluate the six graph operations with their agreed cleanup/reporting interfaces; explicit O-authorized scenario compositions may exercise removal followed by fresh addition, preserving closure and reuse rules. Defer migration, live replacement, in-place rebinding, reliable application channels and autonomous recovery. Add further operations as extensions to the current model only after cleanup and the other baseline mechanisms have been validated through Q14 formal checks and concrete lifetime/resource/failure tests. Extensions require their own state/transition and obligation checks; no generic extension framework or baseline implementation of these capabilities is required.

### Planned transport scheduling increment

**Planned on 20 October 2026; implemented and functionally checked on 21 October
2026. Target timing/resource qualification and final Q14 coverage remain open.**
See [the implementation record](#2026-10-21-two-port-transport-scheduling) and
`verification/graph_core/transport_scheduling_verification.json`.

Extend R04 and the shared host with separate application and management
UDP sockets, bounded application preference, existing outgoing endpoint
round-robin selection, and recurring management/artifact service. Retain
Q18 best-effort semantics and Q14 lifecycle ownership. Implement the core
and simulator changes as a P8 increment; qualify resource and timing
behavior under R05/P9 and include the final scheduler in Q14 checking.
This engineering work can proceed independently of R07/R08 ventilation
decisions. Numeric scheduling settings below are provisional; workload
latency acceptance still needs declared targets.

#### Baseline before this increment

- `graph_core/ports/host_udp.c` uses one nonblocking UDP socket for all traffic
  and reads at most one datagram per host-loop iteration.
- `graph_core/kaos_host.c` advances lifecycle decisions, handles input, and
  submits immediate replies, artifact traffic, management traffic, then a
  bounded application-data batch.
- `graph_core/kaos_endpoint.c` already selects outgoing messages round-robin
  across eligible sender endpoints, with FIFO order inside each endpoint.
  The cursor persists between batches.
- Artifact downloads use sequential CoAP block requests, initially requesting
  256-byte blocks, with one active artifact exchange. Waiting for a response
  does not block the host.
- ESP32 runs the host at priority 3 and WAMR workers at priority 2. Both
  management and application-data submission execute in the host task.
- Application data is best effort: dequeue precedes submission, failed
  submission does not requeue, and a full receiving endpoint drops arrivals.

#### Scope and guarantees

Use two transport classes:

| Class | Socket contents | Service policy |
|---|---|---|
| Application | Channel data frames | Preferential bounded receive and transmit batches |
| Management | Graph operations, reports, queries, acknowledgements, artifact exchanges | Recurring bounded service; pace artifact requests separately from essential management output |

Separate sockets provide separate receive queues and permit explicit receive
selection. They do not reserve radio airtime, isolate all network memory, or
assign Wi-Fi priority. Lower network layers remain shared.

Retain one host task as the owner of graph transitions and transport
scheduling. Keep existing task priorities for the first implementation.
Do not introduce a task or socket per endpoint.

Fairness remains per outgoing endpoint and per message, not per application,
byte, or radio airtime. Incoming application packets share the application
socket queue; the host cannot round-robin unread packets by endpoint.
Per-endpoint buffering after decoding does not protect socket capacity from
a noisy producer. Producer rate admission and stronger ingress isolation are
separate extensions.

Preserve existing application delivery semantics. Add failure counters, but
do not silently introduce retries or promise delivery because service
opportunities are fair.

#### Transport interfaces and configuration

1. Add an explicit application/management class to the host transport
   boundary. Sending selects a class; received packets retain the class of
   the socket on which they arrived. Keep device IDs in the host and socket
   addresses in the platform adapter.
2. Separate waiting for readiness from nonblocking reception by class.
   Waiting observes both sockets, the wake descriptor, and the earliest
   deadline; the host decides how much work to take from each class.
   Readiness may become stale, so a receive attempt must be allowed to return
   empty without blocking.
3. Extend peer routes to contain explicit management and application
   addresses. Authority and collector routes may be management-only.
   Validate unique attribution for each class and reject missing required
   application routes. Source IP and port checks remain configured-peer
   attribution, not authentication.
4. Bind two distinct local ports. Send each class from its corresponding
   socket so source-port attribution remains consistent. Preserve the
   management port where practical; configure the application port
   explicitly rather than assuming an adjacent port is available.
5. Reject application frames arriving on the management socket and management
   frames arriving on the application socket. Retain payload, binding,
   generation, permission, and peer validation after classification.
6. Bound resources for both sockets. Close both on shutdown and on partial
   initialization failure. Preserve wakeups issued before waiting, including
   stop requests and work remaining after a batch.

Update `kaos_host_platform.h`, `host_udp.h`, `host_udp.c`, POSIX arguments,
ESP-IDF Kconfig and startup, test adapters, launch scripts, and readiness
output together. This is a transport configuration change; it need not alter
the data-frame or CoAP payload schema. Treat old single-port launch
configurations as incompatible and report a clear configuration error.

#### Host scheduling policy

Implement explicit service rounds. One round performs the following:

1. Apply due lifecycle and timeout decisions before admitting queued data.
2. Receive and dispatch a bounded application batch.
3. Receive and dispatch a bounded management batch. Apply each accepted
   management message's lifecycle consequences before subsequent data work.
   Keep replies associated with their management socket.
4. Submit a bounded application batch using the existing endpoint cursor.
5. Service immediate management replies and a bounded pass of due management
   exchanges. If work is deferred between rounds, retain a cursor so the
   same peer cannot always win.
6. Service at most one eligible artifact request or retransmission, subject
   to artifact pacing.
7. Perform bounded maintenance, calculate the next deadline, and wait only
   when no runnable work remains.

Preserve the existing rule that an applied removal closes access before
subsequent channel service. Priority selection does not make a removal
effective before its request has been read and processed.

Use finite configurable packet budgets for each phase, with optional elapsed
time limits checked between packets. Count malformed, rejected, and unknown
peer datagrams against receive work budgets so they cannot create an
unbounded drain loop. A time limit cannot interrupt an individual handler.

As an initial tuning profile, use up to four application receives and one
management receive per round; retain the current application transmit cap
of `CORE_MAX_ENDPOINT_RESERVATIONS`. Retain a bounded full peer pass for
management output. These are starting values for verification, not admitted
latency guarantees. Skip empty phases so a quiet class does not waste work.

Every completed round must offer management service even under continuous
application load. Retain due timer handling independently of socket
readiness. This provides a bounded number of application operations between
management opportunities; a wall-clock bound additionally requires bounded
handler execution, lock waits, and CPU scheduling.

Artifact pacing must limit requests for new blocks and constrain repeated
attempts without renewing CoAP or operation deadlines. Give eligible
artifact work a recurring opportunity even during continuous application
traffic. Include the next permitted artifact-send time in the wait deadline,
and avoid repeatedly waking on an already-due retry that pacing prevents.
If an operation allowance expires while paced, preserve its normal failure
and cleanup behavior.

Essential management and artifact responses still share one receive queue.
Pacing requests limits expected artifact response pressure; it does not let
the host select an essential management packet from behind unread artifact
packets. Record this remaining limitation.

#### Artifact checksum performance candidate

**Examine the artifact checksum if application latency becomes an issue
during verification. It is also an explicit candidate for optimising system
performance. Optimisation is not a prerequisite for the initial socket and
scheduling change.**

Currently, `core_artifact_receive()` in `graph_core/kaos_artifact.c` computes
CRC32 across the entire artifact when the final block arrives. This executes
synchronously through the host receive handler with the registry lock held.
The host cannot receive another packet during the calculation, and guest
channel calls requiring that lock must wait.

Measure checksum execution time and registry-lock hold time for several
artifact sizes, including the supported maximum. Correlate final-block
arrival and checksum completion with application latency, queue overflow,
and guest channel-call waiting time. Separate this completion-time effect
from sustained radio and packet-processing contention during the download.

If measurements justify optimisation, evaluate:

- Incremental CRC updates for each newly accepted block, followed by a final
  comparison. Duplicate blocks must not be counted again, and restart must
  reset the checksum state.
- A worker or bounded incremental verification phase, with explicit ownership
  of artifact storage and cancellation behavior.
- A faster CRC implementation that preserves the existing checksum result,
  assessed for both execution time and remaining lock occupancy.

Any optimisation must preserve validation before runtime handoff, deadline
checks, duplicate/restart semantics, and safe cleanup. Moving computation
outside the lock requires a lifetime claim and revalidation before publishing
completion. Do not release artifact storage while verification still uses it.

#### Implementation sequence

| Step | Work | Completion evidence |
|---|---|---|
| 1 | Add transport classes, two sockets, route validation, nonblocking receive-by-class, and configuration changes | Both classes route correctly; invalid configuration and partial startup clean up safely |
| 2 | Add service rounds, packet budgets, wake/deadline integration, persistent fairness, and artifact pacing | Deterministic scheduling tests establish recurring service and lifecycle ordering |
| 3 | Update simulator, relay, authority routing, launchers, and fixtures | Distributed operations and application workloads run through the two-port topology |
| 4 | Add bounded counters and timing observations; run contention experiments | Comparable baseline and changed measurements, including artifact checksum timing |
| 5 | Tune budgets and pacing; document supported operating conditions | Recorded selected settings, workload limits, remaining interference, and verification results |

In `sim_env`, update `graph_transport.py` to target management addresses and
extend `graph_relay.py` to distinguish participant and traffic class.
Preserve source attribution on forwarded packets. Update the process runner
and all tests that assume one address per participant.

Keep directed-link impairment budgets shared across the two traffic classes
unless an experiment explicitly configures separate resources. Giving each
port an independent full link budget would artificially double simulated
capacity and conceal contention. Preserve aggregate loss, delay, and queue
accounting while adding class-specific observations.

After implementation, update `guide/architecture.md`, `guide/usage.md`,
and `guide/verification.md`; record the implemented contract and evidence
in `implementation_rails/` and update the remaining information and implementation gates in this plan.

#### Verification and acceptance

Extend the existing host, UDP, scheduling, endpoint, artifact, and simulator
tests rather than creating a separate transport test framework.

| Scenario | Required evidence |
|---|---|
| Both sockets continuously ready | Application receives its configured preference; management receives its opportunity every round |
| Continuous application output | Endpoint round-robin persists across rounds; management and artifact work continue |
| Unequal endpoint rates and message sizes | No eligible outgoing endpoint is skipped indefinitely; distinguish packet fairness from byte share |
| Artifact download with periodic application traffic | Application service continues throughout download; record delay and losses during blocks and final checksum |
| Continuous management or malformed traffic | Receive budgets remain bounded; application and timer service continue |
| Removal or expiry during queued data | No data operation bypasses the permission change after it is applied; deadline precedence is preserved |
| Full endpoint or socket buffers and send failures | Bounded resource use and observable failures; no new delivery claim or unbounded retry queue |
| Artifact loss, duplicate blocks, timeout, and restart | Existing exchange semantics and deadlines remain valid under pacing |
| Exhausted batch followed by no new packets | Pending work resumes without depending on a new external wakeup |
| Two-port relay and direct UDP runs | Correct class routing, peer attribution, aggregate impairment accounting, and shutdown |

Use deterministic fake-clock tests to check exact service order and maximum
rounds between opportunities. Use real sockets for routing and resource
cleanup. Exercise both static capacity profiles and retain existing graph
operation regressions.

Measure on ESP32 as well as POSIX: application-only baseline, concurrent
artifact transfer, management churn, and combined saturation. Record:

- Per-endpoint application latency distributions and worst observed delay.
- Host receive-to-endpoint enqueue time, guest `Get` delay, and local
  `Put`-to-submission time as separate boundaries.
- Endpoint overflow, submission failures, and available network/relay loss
  evidence. Do not infer the location of a loss from a missing sequence alone.
- Maximum gaps between management service, timer lateness, artifact progress,
  and graph-operation completion or timeout.
- Host-round duration, checksum duration, lock hold/wait time, and application
  worker progress under sustained host load.

Use bounded instrumentation; record its overhead and dropped observations.
Cross-device latency needs a stated clock-error model; use local measurements
where synchronization is unavailable. Successful `sendto()` is not a radio
transmission timestamp.

Before performance acceptance, declare the target application rates,
endpoint counts, message sizes, latency/loss limits, and management/artifact
progress targets for the selected workload. Compare identical conditions
against the single-socket baseline. Report overload behavior separately from
the admitted operating range. Functional completion requires the scheduling
and protocol checks above; a claim of adequate application latency additionally
requires the declared workload targets to pass.

### Core increment on 6 October 2026

The independent Q10 predicate prerequisite now lives in `graph_core/include/kaos_predicate.h` and `graph_core/kaos_predicate.c`, compiled by both host and ESP-IDF builds. Two calls validate/copy a caller-owned expression and evaluate it against a channel bitset. The internal representation is postfix with channel references, constants and binary AND/OR; it is not a selected CBOR encoding. Initialization checks the entire expression, declared references, token count, actual tree depth and `P(Channels)` before modifying the destination. Evaluation and initialization use fixed stack storage, without allocation or recursion. The validated expression and declared channel ordering must remain immutable for the operation; lifecycle logic must supply complete prepared/active or possible channel sets.

The provisional limits are 16 declared channels, 63 tokens and depth 16, counting leaves as depth one. The predicate record occupies 127 bytes. Prefix and indexed-tree alternatives were compared; postfix avoids child indexes and pending-operator bookkeeping. These limits are prototype inputs, not measured board capacity. This increment does not select wire fields, implement admission or construct qualifying sets from endpoint evidence. P0/P1/P2 and the six operations remain incomplete.

Existing test cases and verification models remain unchanged. Standalone boundary/sanitizer checks and an independent tree-based differential oracle exercise this increment; their commands, source hashes, results and qualification limits are recorded in `verification/graph_core/predicate_verification.json`. The temporary checks are not registered repository regression tests. Physical execution, scheduling measurements and final Q14 verification remain pending.

### Local reservation increment on 6 October 2026

`graph_core/include/kaos_admission.h` and `graph_core/kaos_admission.c` add the local resource-reservation prerequisite for no-edge node addition. Two fixed instance slots have stable lifetime ledgers and preparation completion mailboxes; four separate operation records each retain the original opId, acceptance deadline and 128 bytes of reserved report storage. The instance ID is derived from the creation opId. All ID values, including zero, are represented without a sentinel identity. With this specialization, a retained same-instance submission is a duplicate, not an independently supplied conflicting target.

One serialized `core_node_admit` decision discards retained duplicates before checking a fresh allowance, checks deadline arithmetic and every local capacity, then reserves the instance and operation/report record together. The caller supplies a positive fixed per-instance byte quota and total byte budget once at pool initialization; each lifetime enforces that same quota. Admission performs no allocation or dispatch. Failure leaves the pool and output unchanged. Fixed quotas avoid adding a mutable lifetime-limit interface. Same-ID retries preserve the original deadline and generate no new result.

`core_admission_release_node` requires a closed, drained, uncharged lifetime and an empty completion mailbox; services must also release their associations outside that ledger. It frees instance capacity while retaining the operation/report record. Slot reuse reopens the original lifetime without resetting its access-ticket sequence; exhausted slots cannot reopen. `core_admission_forget` additionally requires prior node release and a caller-established Q06 eligibility decision. It does not validate ACKs or implement eviction. This conservative slice retains a creation record while its node remains live; independent retirement of a successful live node's report remains future work. Once both ownership and history have been retired, old construction can be admitted again under the explicit Q05 limitation.

This is an internal reservation boundary, not complete P2 protocol admission. Request/schema validation, O's authoritative empty-incidence fence, complete immutable request storage and qualified report/revision budgets remain integration requirements before managed preparation is enabled. The reserved report bytes are not encoded evidence, and the deadline must be created at protocol acceptance rather than at an earlier provisional hold. No STARTED/CLEANED or operation outcome is manufactured by these resource primitives. The fixed pool occupies 912 bytes on the host and 832 bytes with the current ESP32 compiler, in addition to the configured runtime byte budget; these are static layout measurements, not a qualified workload profile.

Existing tests and models remain unchanged. Standalone sanitizer/race checks and an independent ownership/retention map oracle exercise admission, rejection atomicity, resource release, retained duplicates, reuse, stale completions and integer bounds. Commands, source hashes, counts, temporary verification sources and limitations are recorded in `verification/graph_core/admission_verification.json`. Both platform builds include the same source; hardware execution and final Q14 verification remain pending.

### P2 implementation on 7 October 2026

The [P2 contract and walkthrough](graph_p2_contract.md) now specifies six bounded CBOR request variants, explicit port manifests, shared host/device limits, coordinator and owner admission, complete-incidence fences, cumulative evidence and revision budgets, CoAP resources, and artifact ownership. `core_admission` was extended rather than introducing a second registry. The original resource-only API remains available; public protocol admission validates the complete request and origin before atomically reserving node/endpoint/operation/report capacity. Current and projected incidence is limited to four bindings per instance so complete node removal remains representable.

O now lives separately in `../sim_env/sim_env/orchestrator.py`, with immutable wire descriptors in `graph_protocol.py` and a UDP client/artifact server in `graph_transport.py`. It owns the non-recycled ID sequence, atomic authorization and incidence/removal fences, immutable dispatch, unsent withdrawal, exact pre-work rejection and cumulative-result reconciliation. It does not manufacture K transitions. This supersedes the earlier note that O allocation and all request/schema work were still pending.

The C core provides all-six request admission, independently issued owner reservation/cancellation, bounded CoAP exchanges and reassembly, full-snapshot query results, and checked Block2 staging with length/CRC validation and explicit handoff. The new host exercise port runs those real interfaces, including local failure cleanup, without loading guests or activating endpoints. P2 artifact readiness is not protocol READY or STARTED. P3–P6 still supply operation execution and owner-evidence publication; P7 supplies collector/ACK/eviction integration. Report and cleanup capacity are reserved from admission, with at most 78 aggregate revision advances in the selected nine-object profile.

New C/Python and real UDP process regressions, a finite admission/lifetime refinement, source hashes and static host/ESP32 measurements are recorded in `verification/graph_core/p2_verification.json`. All pre-existing test cases and models are preserved. P2 implementation and host contract verification are supplied, but its physical target qualification remains open: no board was connected, the foundation firmware only compiles P2 objects, and WAMR peak overlap/radio/stack/timing measurements cannot be inferred from host tests or static layout. R05 and the final Q14 model remain explicit qualification work; this increment does not mark P0/P1 or P3–P9 complete.

### Object evidence simplification on 8 October 2026

Q07 now orders owner evidence by containment of cumulative facts, removing all per-object revision counters. Equal sets repeat, strict subsets are ignored, strict supersets advance and incomparable sets conflict without changing retained state. The C report stores one byte per object; O receives the same flat fact list. Kc retains its aggregate revision and exact storage-ACK identity, with the existing 78-advance maximum. Schema 2 is used consistently for requests, queries, both report kinds and storage ACKs; schema 1 is rejected. No lifetime, admission, outcome or cleanup guarantee changes. Existing execution, collector and physical qualification gates remain open. Historical verification files keep their original schema/source hashes; current checks are recorded separately.

### Universal dynamic binding on 9 October 2026

Every node supports dynamic binding. Remove `allow_dynamic`/`dynamic_ports` from O descriptors and K scope/instance records, and remove the final capability flag from node-add bodies. Schema 3 accepts exactly nine node fields, or eleven with channels/predicate; all management messages share this version. Node availability/success, identity, declared-port compatibility, removal fences, incidence and capacity checks still apply. This changes the default capability, not who authorizes a new binding or when endpoint work may execute. Prior schema-1/schema-2 verification snapshots retain their original source hashes.

### P3 host runtime increment on 9 October 2026

The [P3 contract and walkthrough](graph_p3_contract.md) records the current no-edge implementation. `kaos_node.c` drives kinds 1 and 4 through artifact handoff, inert runtime preparation, commit/start authorization, serialized INIT/exit acceptance, strict deadlines and full cleanup. A small `prepare`/`lock`/`unlock` runtime boundary uses one retained mailbox and worker ticket per node; the existing admission records remain authoritative. The Linux x86_64 WAMR adapter supplies two independent workers, bounded arenas, checked guest entry, termination and disposal. The UDP source builds a separate `kaos_p3_host`; endpoint and multi-channel execution remain P4/P5 work.

WAMR is pinned to `b124f70345d712bead5c0c2393acb2dc583511de` with a reviewed build-copy correction for concurrent cluster lookup/final execution-environment removal. The restricted bytecode profile rejects implicit initialization; the guest ABI is explicit configuration/main plus empty INIT, reporting-only ERROR and bounded cancellable event wait. Native worker stacks and runtime-global storage are separate provisional host overhead, not included in a node's artifact-plus-arena request or presented as an ESP32 capacity result. The corrected host build passed 13 C suites with address/undefined-behavior and leak checks, and ThreadSanitizer passed runtime lifecycle tests plus 200 concurrent cancellation/disposal cycles. ASan fake-stack mode is disabled only for WAMR's native-stack checks; that coverage limitation and a ThreadSanitizer startup mapping failure before its successful run are recorded explicitly.

The approved schema-3 fixture migration closes the earlier dynamic-binding test gap; its complete pre-P3 host suite results are recorded separately in `dynamic_binding_verification.json`. The new finite `NodeRuntime.tla` configuration passed nine invariants and six properties over 496008 distinct states, with a concrete handler/test mapping and queued-INIT/exit witness. Its conditional liveness assumes fair timers/control, worker termination/disposal and access drainage; it does not prove runtime memory safety, physical latency, slot reuse or separate creator/removal report deadlines.

The shared engine cross-compiled in the ESP-IDF foundation project; that fixture does not execute WAMR. Full Python discovery ran 86 tests: 66 passed, including nine new real UDP/WAMR cases, and 20 pre-existing opt-in integration cases were skipped. Source hashes, commands and qualification limits are in `verification/graph_core/p3_verification.json`. An ESP32 runtime adapter, physical execution and memory/stack/scheduling qualification remain open. These are explicit P3/R05 gates: this section records implementation progress and does not mark all of P3 complete. P4–P9 and collector/ACK/eviction integration retain their planned scope.

### P3 target completion and qualification corrections on 9 October 2026

The P3 no-edge implementation now runs on Linux and the physical original ESP32 without PSRAM. `graph_core/runtime` builds the same restricted WAMR adapter for both. ESP-IDF uses two pthread workers with fixed 16 KiB stacks and retained semaphore wakeups; the higher-priority control task remains independent of compute-only guests. The target fixture links real admission/node execution and exercises add/remove, failed startup, expiry, tracked-access cutoff and late full cleanup. Two 88 KiB runtime arenas fit the measured no-edge profile; repeated lifecycle cycles restore the warm heap baseline.

Bring-up corrected two pre-existing host claims: WAMR's default linear-memory mmap path bypassed the charged arena, and its classic interpreter produced recoverable UBSan alignment diagnostics even in the earlier host build. Usage-aware allocation now includes linear memory; exact-match build copies correct pointer alignment, and sanitizer builds fail on undefined behavior. ESP32 allocation additionally requires explicit eight-byte alignment. Existing test cases and models remain unchanged; a new budget regression catches the allocation escape, and a separate finite semaphore model checks retained wakeups.

The [P3 contract](graph_p3_contract.md) and [target verification record](../verification/graph_core/p3_target_verification.json) contain the build, measured profile, corrections, source hashes and test evidence. This completes the scoped P3 no-edge runtime/operation slice. R05's combined two-node/one-channel Wi-Fi profile, distributed target qualification and final P4–P9 configuration remain open; no broader embedded capacity or hard real-time claim follows from this fixture. Historical verification files are retained and must be read with these corrections.

### Four-node capacity target on 9 October 2026

The user raised the viability minimum to four concurrent nodes and requested investigation of eight endpoints per node, using Beacon examples as workload evidence. This supersedes the earlier two-node minimum for future acceptance; it does not retroactively extend the P3 qualification. The production profile remains two node slots/eight endpoint reservations/four bindings per node pending explicit profile and protocol qualification.

An isolated build-copy experiment passed four concurrent compact guests with 25600-byte arenas, unchanged 16384-byte native stacks, 32 allocated 1024-byte endpoint-budget buffers, 32 endpoint records and eight operation records. It retained 41448 bytes of free internal heap. This is runtime residency plus memory reservation, not endpoint or Wi-Fi execution. New guest linker exports enable the pinned WAMR's existing shrinking feature; the measured single-fixture arena threshold was 25184 bytes on ESP32. The experiment also exposed and corrected an uninitialized temporary vector on WAMR's OOM cleanup path. Production limits and existing test cases were preserved.

The [capacity evidence](../verification/graph_core/capacity/results.json) and [P3 capacity discussion](graph_p3_contract.md#four-node-capacity-experiment) capture Beacon artifact sizes, reproduction commands and limits. Before accepting four nodes/eight endpoints, P4 must demonstrate actual full queues and traffic with Wi-Fi and management/cleanup headroom, and enlarge/check incidence, ports, report object/revision limits and bounded request encoding together. Artifact size alone is not a runtime memory budget.

### P4 implementation and local capacity evidence on 10 October 2026

The [P4 contract](graph_p4_contract.md) records independent endpoint preparation,
receiver-first edge activation, scoped removal, copied nonblocking data calls,
per-role FIFO semantics, exact binding attribution and coalesced channel-state
notifications. A separate `kaos_p4_host` integrates these services with real
CoAP management and best-effort UDP data. Schema 4 adds activation/owner commands
and the guest channel ABI; only explicitly approved version literals changed
in existing test cases. Multi-channel construction, full distributed node
removal and collector/ACK integration retain their P5–P7 scope.

The optional four-node profile jointly selects four node slots, 32 endpoint
roles, eight incident channels/ports, 24 operation records, 17 report objects
and the corresponding bounded wire/revision limits. All 20 host test entries
pass, including new local/remote Python-to-WAMR process cases. The physical
ESP32 without PSRAM now passes three complete local four-node/eight-endpoint
cycles with all queues full, 256-byte validated traffic and full reclamation.
The new target fixture required the same control-priority-above-workers setting
as P3 to avoid starvation when guests leave notifications unread.

The [measured memory breakdown](graph_p4_memory.md) and
[verification record](../verification/graph_core/p4_verification.json) separate
119,880 bytes of static DRAM, 26 KiB node arenas, native stacks, allocator
overhead and the 39,588-byte free heap under full load. The global arena reserves
32 KiB but uses only 40 bytes in this workload; candidate reductions are recorded
for a decision and have not been applied. This establishes the local runtime
and endpoint baseline. Wi-Fi plus management/cleanup headroom, representative
application guests, distributed physical qualification and the final composed
TLA+ configuration remain open. It does not satisfy the full networked
four-node acceptance gate above or mark P9 complete.

### P5 implementation and host qualification on 11 October 2026

The [P5 contract and walkthrough](graph_p5_contract.md) documents multi-channel
node addition in the shared node/edge services. The same immutable Boolean
predicate gates complete prepared and active channel sets. The runtime and
local endpoints prepare independently; receiver activation precedes sender
permission; startup dispatch, actual worker entry and STARTED acceptance each
retain their own guards. Before dispatch, alternatives remain usable while P
is possible. During Starting, known loss of P selects whole cleanup.

Pending optional channels retain the original admission-based deadlines after
historical Succeeded. Their pair cleanup, missing owner obligations and late
closure do not revise node success. Compact indexed commands independently
reserve/cancel remote roles under the original creation ID, including
cancellation before preparation, while keeping the existing 160-byte transport
effect buffer. One shared whole-cleanup cutoff prevents node/channel service
scheduling from extending an attempt. The unused per-record report byte array
was removed; existing payload, queue and runtime-arena limits are preserved.

New deterministic, real WAMR, real UDP process and four-node capacity tests
cover the P5 implementation. All pre-existing test sources remain unchanged.
The [P5 verification record](../verification/graph_core/p5_verification.json)
records results and hashes. The ESP32 P5 fixture cross-compiles but no serial
board was connected, so new physical execution and networked resource
qualification cannot be claimed. P5 implementation/host evidence does not
close the preceding four-node Wi-Fi acceptance gap or P9's mandatory final
formal/target qualification. P6 full removal and P7 collector work remain open.

### P6 implementation on 11 October 2026

The [P6 contract and walkthrough](graph_p6_contract.md) describes full-node
removal through the existing node/edge services, indexed remote cleanup,
retained remote incidence after local reclamation, and independent overlapping
removal observers. The wire buffer and production structure sizes are unchanged.
New deterministic, real UDP/WAMR and four-node full-queue removal tests pass;
the ESP32 fixture cross-compiles. No new physical, Wi-Fi or final formal run
is claimed. P7 collector/retirement integration remains separate.

Host regression qualification now passes all 28 entries. With explicit user
approval, `complete_incidence_bound` supplies remote CLEANED evidence after
local endpoint release, before its existing successful-admission assertion.
The other 41 pre-existing verification sources remain unchanged. The separate
four-node suite passed all seven entries. The
[P6 verification record](../verification/graph_core/p6_verification.json)
records the approved correction, results and remaining qualification limits.

### P7 implementation on 12 October 2026

The [P7 contract and walkthrough](graph_p7_contract.md) completes exact storage
ACKs, explicit missing-history queries, bounded eligible retirement and the R06
host RAM collector. Current reports become visible again when later evidence
advances their revision. Live runtime, optional work and unresolved closure
remain protected. Settled Ki records retain their rejection fences until a
successful admission needs capacity; coordinator records may retire after exact
O/DB acknowledgement. Slot/incarnation guards and bounded final-owner reply
receipts protect callbacks and response loss across retirement. Receipt ordering
uses the supplied ports' increasing, non-wrapping MIDs and NSTART=1.

P6 fresh removal attempts continue to share physical cleanup with independent
observer scopes and deadlines. New tests cover direct/archive reordering,
obsolete ACKs, DB full and DB/O outages, report pressure, late optional facts,
owner response loss and slot reuse. Existing test cases are unchanged from the
P7 session baseline. The [P7 verification record](../verification/graph_core/p7_verification.json)
records the full host/four-node results, ESP32 cross-build and 848-byte aggregate
increase in the measured four-node target structures. The physical ESP32 P7
fixture subsequently passed all three full-capacity lifecycles on 12 October,
with zero final reservations and repeatable heap recovery. Distributed Wi-Fi
and final composed TLC qualification remain open. Q05 replay after discarded history remains
an explicitly characterized limitation; P8 applications and P9 final target/formal
qualification remain separate.

### Shared host consolidation after P7

The full host now uses `graph_core/kaos_host.c` for the main loop, scheduling,
module wiring and artifact/runtime handoff. Artifact CoAP handling extends
`kaos_artifact.c`; outgoing management exchanges extend `kaos_control.c`; edge
reply lifetime guards and node artifact-failure decisions remain in their
existing operation modules. POSIX and ESP-IDF entrypoints share a bounded BSD
socket adapter for address attribution, I/O, waiting, retained wakeups and locks.
There is no second host-specific artifact or transport engine. The canonical
executable is `kaos_host`; the P2 artifact-only executable moves to verification,
and old P3/P4 build entrypoints run the complete shared implementation.

This implements the narrow-port/shared-core architecture above without changing
schema 4, the guest ABI, outcome rules, original cutoffs or P7 retention policy.
The host context remains fixed-address and WAMR remains one instance per process.
Shutdown remains a process boundary, not graph closure evidence. Existing test
assertions are preserved; the old P2 private-function test changes its fixture
include path only. New host-interface and adapter tests complement the existing
UDP/WAMR operation scenarios. Details and source hashes are recorded in
[host_refactor_verification.json](../verification/graph_core/host_refactor_verification.json).

P8 should use the canonical executable and shared production loop. R07/R08
application decisions remain independent. P9 must qualify the networked ESP-IDF
host's memory, stack, scheduling and distributed Wi-Fi behavior and map this
scheduler into the final Q14 model. Cross-compilation does not close those gates;
prior local fixture measurements do not qualify the new networked executable.

The subsequent scheduling refactor keeps these same tasks. Paired lifecycle
passes are scheduled explicitly by `core_host_step`, before and after input and
after staging release. WAMR entry guards are read-only; workers no longer run
graph lifecycle passes. A node cleanup check precedes edge work so channel
allowances cannot extend an expired node's startup. Two bounded edge/node rounds propagate local readiness,
commit, activation and dispatch at each checkpoint. Input consequences remain
serialized before worker entry, and output follows the control decisions.
Named eligibility/authorization checks improve locality within the existing
host and node modules. The [scheduling guide](guide/architecture.md#scheduling-and-task-responsibilities)
and [verification record](../verification/graph_core/scheduling_refactor_verification.json)
describe the ordering and tests. This changes neither the task topology nor the
remaining P8/P9 qualification scope.

### P8 protocol runner increment on 15 October 2026

`../sim_env/sim_env/graph_run.py` now owns fresh process experiments around the
canonical `kaos_host`, the existing O authorization API and CoAP transport. Its
CLI runs all six operation kinds, including complete removal before adding a
fresh binding. Per-coordinator exchange locks allow independent concurrent
submissions without a global lifecycle wait. Accepted exact report bytes are
logged before storage ACK; missing history and transport timeout preserve O's
uncertainty. Process teardown never supplies graph closure.

`graph_relay.py` uses one proxy socket per participant, preserving enrolled
source-address attribution without firmware changes or relay envelopes. It
provides directed-link loss, fixed delay and duplication,
scripted reordering through delay changes, a bounded pending queue and records
policy changes, actual packet decisions, delivery lateness and teardown drops.
Existing descriptors remain the experiment interface; no new scenario schema
or extension framework is introduced. Legacy LOAD execution is not selected.

New relay and real-host tests preserve every existing test case. The evidence
record is `verification/graph_core/p8_verification.json`. This increment leaves
R07/R08 application choices, named transition barriers, full observation-loss
accounting, runner-driven collector/crash scenarios and final P9 target/formal
qualification open. Seeded per-link decisions do not make OS process scheduling
deterministic. No physical ESP32 or new formal qualification is claimed.

The user subsequently clarified that every packet on a directed link must share
the same policy. The relay now keys policy and random streams by `(source,
destination)` only. Traffic-class labels remain diagnostic, and reverse
directions remain independently configurable for asymmetric loss. With explicit
approval, the affected P8 tests now check shared impairment and cleanup after
restoring a failed link within the original allowance. The original P8 record remains historical;
`verification/graph_core/link_policy_verification.json` records the correction.

### P8 workload increment verified on 20 October 2026

The restricted-ABI pipeline and feedback fixtures now run through the existing
O/GraphRun/production-host path. Pipeline verifies 64-byte sequence/value/padding
samples and `2*value+1` on three devices and co-located in the four-node profile.
The distributed script requires successful removal of the original transform
and its complete incidence before fresh addition, observes the replacement,
then explicitly removes all nodes. The local script removes the transform and
records source/sink as remaining at teardown: its four ordinary admissions fill
the existing CoAP reply cache. No profile/cache limit was increased.

Feedback uses one outstanding sequence with idempotent fixture retries and
checks correlated responses locally and remotely. The remote case delays
authority-to-initiator preparation and impairs the forward device link after
startup. All packet types share each directed link policy; reverse directions
remain independent. Faults are removed before the explicit cleanup phase.

An optional shared-host callback observes successful guest PUT/GET boundaries;
the guest ABI is unchanged. POSIX uses bounded nonblocking stdout writes with
per-host sequence numbers and terminal attempt/drop counters after worker join.
The evaluator marks missing observations inconclusive, verifies payloads and
matching source-instance PUTs for GETs within each window, and uses a synthetic
minimum of eight distinct output samples per 250 ms window. GET does not prove
subsequent guest acceptance or actuation. These host-clock observations neither
expose oracle time to guests nor qualify acquisition age or embedded timing.

Replacement exposed a sparse removal admission bug under record pressure:
`check_removal_binding_conflicts` received the candidate eviction slot rather
than the retained record for the current operation. Its old scope could reject
a valid fresh removal of the same logical channel. Passing `found` preserves
all retained descriptor checks and failure atomicity. The new
`admission_reuse_test.c` reproduces reuse and checks that changed descriptors and
duplicate entries in the current removal remain rejected without mutation.

The default suite passed 40/40 entries on 16 October (retained summary; its
temporary binary hashes/detail log were not captured before interruption).
The fresh four-node suite passes 14/14 entries, including all ten workload
cases, on 20 October. Ruff lint/format pass. All 81 C/H/Python verification
sources in the 16 October checkpoint match their hashes. No existing test case
was edited to close this increment. Commands, source/artifact hashes and limits
are in `verification/graph_core/workloads_verification.json`; the human code
walkthrough is `guide/workload-walkthrough.md`.

APP01 remains partial: periodic sampling, scheduled-miss accounting and explicit
synthetic deadline/freshness evaluation are not implemented. APP02 has the
declared fixture coverage, not a general application delivery guarantee.
Named lifecycle fault barriers, broader loss accounting, crash/freeze/collector
experiments and application alternative-channel examples remain P8 work.
Ventilation behavior/model/service acceptance remains gated by R07/R08; no
new physical target or composed formal qualification is claimed. Next implement
the remaining deterministic application measurement slice after resolving its
minimal clock/scheduling interface against Q19; do not infer periodicity from
finite `event_wait` calls or infer freshness from successful graph startup.

### P8 offline timing increment verified on 20 October 2026

Compared two independent designs before implementation. A guest clock or new
WASI time import would violate the current Q19 baseline. Relative event_wait
cannot establish a periodic schedule or count scheduled acquisition misses.
Externally paced input requires additional peripheral/source plumbing.
Selected the smaller existing-trace evaluator; APP03 remains an independent
next slice. No mandatory timestamp, guest API or production C change was made.

The new sibling `graph_workload_timing.py` exposes
`evaluate_timing(events, devices, workload, phases, *, budget_us)` and an offline
CLI taking a saved run and explicit synthetic budget. It reuses existing
value/instance validation, requires a complete channel trace, and checks ordered
source PUT → expected transform/responder GET → PUT → final GET. Origins are
first successful source publications over the entire trace; retries do not
reset them. Concrete instance and sequence identify the short nonwrapping
fixture's sample; embedded observer times determine order, not JSONL arrival.

Per-window cohorts contain first publications in `[begin,end)` with
`publication + budget < end`. The tail is uniformly excluded even for early
deliveries. Elapsed `<= budget` passes; late or unobserved final GETs count as
budget misses with unknown cause. Missing traces and empty cohorts are
inconclusive. Min/max include delivered eligible samples only. Original graph
reports and the eight-sample criterion remain separate from timing status.
CLI output is exclusive-create and records hashes of all three inputs and both
evaluator modules. Two successful replay invocations produce identical bytes.

Review identified a draft attribution gap: source PUT and transform PUT could
each precede final GET without being correctly ordered with each other. A new
regression reproduced three invalid histories (output before origin, input GET
before origin, absent transform GET). Ordered copy-boundary checks reject all
three. These host observations still do not prove guest acceptance or physical
execution. No acquisition-time or cross-device clock claim is introduced.

Fresh ASan/UBSan CTest results: default 41/41 in 28.03 s; four-node 15/15 in
11.38 s. All 13 new timing checks run in each profile. Ruff lint/format pass.
The 81 pre-existing verification sources match their before-increment hashes.
Only the new test file and CMake registration were added; no existing test case
was changed. `verification/graph_core/timing_verification.json` retains hashes,
commands, configurations, detailed logs and review disposition.

The saved healthy pipeline's value check passes while its synthetic 50 ms
budget fails: each window has 177 eligible publications, with 3/4 lacking an
observed final GET. Preserve that failure as measurement evidence, not a claim
that graph startup implies timed delivery. APP01 remains partial for periodic
acquisition, scheduled misses and acquisition freshness; OBS09 skew/drift is
also open. Next implement APP03 alternatives through explicit operations and
existing guest imports. R07/R08 application acceptance and final P9 target/formal
qualification remain separate gates.

### P8 alternative-channel increment verified on 20 October 2026

APP03 now has a restricted-ABI application fixture with source 10 and sinks
20/30 on three hosts. Source addition carries two outgoing bindings and the
existing `A OR B` predicate. The guest keeps two independent sequence counters,
one sample buffer and cooperative event waits; DISABLED/FULL on one branch
cannot suppress the other. No production C, guest API, pool/cache bound or
protocol semantics changed.

The new sibling `graph_alternatives.py` takes host/guest/output plus `first=A|B`
and a seed. It starts both sinks, partitions source→the other sink before source
authorization, and requires successful source startup with only the selected
endpoint pair ACTIVE. It observes a single-branch window, restores the directed
link, requires a later report with both pairs ACTIVE, then observes both branches.
An explicit removal closes the initially working edge; a third window requires
continued samples on the survivor and no successful copies on the removed edge.
Source and sinks are explicitly removed afterward: seven successful operations.

The first draft wrongly treated an absent post-ACK query as lost live-source
evidence. Both process cases reproduced this. `core_admission_report_retained`
intentionally suppresses an acknowledged unchanged revision while live records
remain; a new revision can be queried when the optional branch advances.
The runner now polls past empty queries/timeouts within its observation bound
and requires actual fresh evidence. Existing GraphRun/core behavior is unchanged.

The evaluator reuses complete channel-trace parsing, validates 64-byte values
and padding, verifies concrete callers and preceding matching PUTs for every GET,
and requires eight distinct sequences per active branch per 250 ms window.
Inactive branches must have zero successful PUT/GET observations. Incomplete
traces remain inconclusive; host copy-out is not guest acceptance or actuation.

Default ASan/UBSan CTest passes 42/42 in 34.98 s; four-node passes 16/16 in
18.10 s. Each runs all ten new tests (eight evaluator cases and A-first/B-first
actual-host flows). All 82 pre-existing verification sources match baseline
hashes; no existing cases were changed. Ruff passes. Separate standards/spec
reviews found no blockers; numeric predicate/report semantics gained comments
without a shared-runner framework. See `alternatives_verification.json` under
`verification/graph_core/` for commands, hashes, retained runs and qualifications.

APP03 late joining and explicit branch removal are covered within these fixture
bounds. Optional-branch expiry/cleanup before joining is not application-tested
by this increment. Next implement APP04 mixed input alternatives with required
output using the same explicit-operation path. APP01 acquisition scheduling and
freshness, OBS09 device clocks, broader lifecycle/failure observations, R07/R08
ventilation criteria and P9 physical/formal qualification remain open.

### P8 mixed-dependency increment verified on 20 October 2026

APP04 now uses four hosts: sources 10/11 on hosts 2/3, transform 20 on host 1,
and sink 30 on host 4. Transform addition carries input A, input B and output
with `(A OR B) AND output`, using existing postfix tokens. The restricted guest
has four roles, one sample buffer and one pending output. Even/odd sequence
identities distinguish the input sources; the transform computes `2*sequence+1`.
FULL retains the pending sample; DISABLED discards it. Existing imports and all
production core/resource bounds remain unchanged.

Two design agents compared a two-case diagonal with the four-case matrix.
Both require fresh runs: partitioning an already prepared channel cannot
retract its READY fact. Selected the four-case A/B × missing-output/inputs
matrix through one runner and two explicit options, with no new scenario
framework. Before release, require positive node/prepared-channel READY evidence,
missing remote endpoint(s), Pending outcome and history 0. After release require
Succeeded/history 39 and complete ACTIVE selected input plus output. Missing
ACKed revisions and timeouts never satisfy the gate.

Each run observes a serving window, explicitly removes the required output,
and observes a second window with continued input consumption but zero output
copies. The unused input path stays partitioned until cleanup. Restore it,
remove the transform with remaining incidence, then remove all peers. All nine
operations must report success before teardown. Initial startup success remains
historical and separate from the second window's failed output-service criterion.

The new sibling `graph_mixed.py` evaluator requires complete traces, exact
caller/owner, source parity, values/padding and ordered source PUT → transform
GET → transformed PUT → sink GET. Both 250 ms windows need eight distinct input
reads; serving needs eight output reads, while the removed-output window needs
zero output copies. Missing observations are inconclusive. The four saved runs
have 123–208 distinct output samples while serving, then zero output copies
with 197–212 distinct input reads after output removal.

Default ASan/UBSan CTest passes 43/43 in 49.15 s; four-node passes 17/17 in
30.70 s. Both execute all 13 new cases (nine oracle, four process). All 84
pre-existing C/H/Python verification sources and 42 production core files
match their baseline hashes. Existing tests were not changed. Ruff and separate
standards/specification reviews pass without blockers. See
`verification/graph_core/mixed_verification.json` for detailed evidence.

APP04's fixture is covered; no physical actuator or application acceptance
claim follows from GET. Simultaneous-input fairness and post-start input failover
are not tested. APP01 acquisition scheduling/freshness, APP03 optional expiry
before join, OBS09 device clocks and broader fault/observation coverage remain
open. APP11 producer/consumer stalls are a possible next deterministic plumbing
slice without choosing ventilation policy. APP05–APP10 application-specific
behavior and acceptance still require R07/R08 decisions; final P9 target/formal
qualification remains mandatory.

### Native imports and remaining deterministic workload checks: 20 October 2026

Implemented the agreed [native-import addendum](graph_native_imports_plan.md)
through Python authorization, bounded request decoding, owned admission/runtime
copies, restricted-profile validation and actual loaded Wasm import-type
checking. The fixed `kaos_native` registry currently exposes one synthetic
bounded sensor function. At most four distinct 1–32-byte names are granted;
no-grant schema-4 bodies remain unchanged. The optional runtime callback is
required for nonempty grants. Cancellation drains the synchronous native call
before worker disposal and CLEANED. No physical acquisition or application
sensor policy is selected by this interface.

APP03 now covers optional expiry before joining in both A-first and B-first
cases. Require successful startup before observing expiry; positively observe
the unjoined branch's expiry and unresolved cleanup, restore its directed link,
then require cleanup while historical startup success and selected-channel
service persist. All six graph operations succeed. The final retained windows
contain 159–171 distinct selected-branch samples each and zero optional copies.

APP11's new real-WAMR/endpoint fixture covers stopped production, stopped
consumption and paused transfer. It observes EMPTY after drain, receiver
drop-incoming at capacity, and sender FULL, then cancels/disposes both guests
and reclaims endpoints. It directly activates established endpoint fixtures;
it does not prove coordinator-history behavior or choose validity/fallback
rules that still depend on R07.

Default sanitizer CTest passes 50/50 entries in 58.04 s; four-node passes 24/24
in 42.36 s. Only the explicitly approved P5 designated-field initializer
changes among 86 pre-existing C/H/Python verification sources. New Python
files and the two extended simulator modules pass Ruff checks/formatting.
The ESP32 four-node P7 firmware cross-build passes. Compiler layouts add
3264 B to admission-pool storage and 544 B to runtime-slot storage; physical
heap/stack/timing qualification remains open. See
`verification/graph_core/continuation_verification.json` for retained evidence
and [current decisions and engineering work](#current-decisions-and-dependencies) for decisions and engineering gaps.

This closes these finite native-import and workload checks, not P8/P9 as a
whole. The planned two-socket scheduler, broader observation/fault experiments,
acquisition/freshness evidence, final Q14 composition and physical target
qualification remain work. R07/R08 continue to gate ventilation acceptance.

### Composed execution model progress: 20 October 2026

Added `verification/graph_core/model/HostExecution.tla` and finite
configurations joining worker startup/native ownership, physical endpoint
state versus accepted facts, strict deadline/cutoff ordering, bounded packet
and data queues, removal, exact revision ACKs and fresh-instance reuse.
Independent review corrected the node CLEANED guard to require reclamation
of incident local endpoints, matching `core_admission_release_node`.

The reviewed no-channel runtime configuration exhaustively checks 18,340
distinct states; the blocked-native configuration checks 2,040. Five expected
reachability traces establish startup, blocked-native removal with unrelated
cleanup, simultaneous native/packet/data pressure, stale traffic across reuse,
and optional expiry overlapping removal. These witnesses show reachable
interactions, not exhaustive acceptance of their larger configurations.

Remote, mixed-predicate, reuse, partition and data-enabled exhaustive searches
remain incomplete. Per-action fairness is assumed; actual control scans,
worker availability and lock waits are not explicitly scheduled. Admission
competition, independent concurrent-operation records and complete collector
retirement still need modeling; abstract data queues do not prove FIFO payload
semantics. Outcome progress permits cutoff/Unresolved rather than guaranteeing
remote cleanup. Q14 is therefore still open. Exact runs, frozen sources,
assumptions and handler mapping are in
`verification/graph_core/model/HostExecution.md` and the continuation record.

### Adoption of the replacement

Use an isolated build target while bringing up the new core. Switch the simulator's device executable and workloads to that target when a vertical slice includes its success, failure and cleanup behavior. Keep earlier artifacts as a comparison baseline where useful; the two implementations need not interoperate or share writable runtime records. When the new implementation meets the conformance gates, remove superseded build paths, wrappers and tests tied only to discarded internals, preserving tests of required observable behavior. Runtime cutover, record conversion and compatibility support are additional requirements only if later requested.

## Test and verification strategy

The [test catalogue](graph_operation_test_plan.md) supplies implementation acceptance cases within the selected scope. Instantiate decision-dependent oracles from the recorded agreements and this plan's R01–R08 gates before running them; its older “Until Q18–Q21 are answered” wording does not reopen settled Q18/Q19 or Q21 plant behavior. Use small deterministic core tests for decision boundaries and ownership; parser/property tests for the selected encoding; real KaOS/WAMR process tests for distributed operation; workload tests for service; and target measurements for resource/timing claims. Retain the existing TLC suites and add implementation-specific admission/lifetime/reuse refinements where abstractions are replaced. Existing tests are reused or translated only when they check retained requirements; do not make the old implementation the correctness oracle for new behavior.

Q14 adds a mandatory acceptance gate: TLA+/TLC safety and liveness checks must cover the final implementation state/transition configuration, including the selected queues, reserved completion capacity, workers, timers, startup-marker convention and Q13 access/reclamation transitions. State fairness, live-host, clock, delivery and termination assumptions; keep Q05/Q08 limitations explicit. Check progress of enabled control/deadline/cleanup work under load and conditional operation outcomes, not unconditional success during permanent failures. Retain model/configuration hashes, finite bounds, properties and traces, with concrete-handler mapping and deterministic trace tests. Existing abstract suites alone do not discharge this gate; runtime memory-safety tests and ESP32 measurements remain required. The final model and checks are deliverables, not completed results.

Map concrete events to model actions: accepted immutable request → AcceptAddition/AcceptRemoval; owner preparation → Prepare/ObserveReady; recorded commit → Commit; activation authorization/owner transition/accepted report → IssueActivation/Activate/ObserveActive; start → Dispatch/Execute/EmitStarted/SucceedAddition; removal → SendCleanup/RecordRemoval/Close/ObserveClosed/FinishCleanup. Names differ between suites; preserve their boundaries rather than requiring a specific C function name.

Every run records source and configuration hashes, profile, clock mode, deterministic schedule or random seed, faults actually injected, expected outcome, observed outcome/evidence, outstanding obligations, memory ledger, event loss, and test duration. A missing event is uncertainty; it cannot prove absence of execution or safe closure. Require explicit reachability of each success/failure history, so safety tests cannot pass merely because nothing starts.

Expected negative model failures have two translations. A guard-removal trace becomes a C regression where the conforming implementation prevents the bad state. An assumption-removal trace becomes a test that preserves safety while declining the unsupported progress claim, usually reporting Unresolved when Kc/timers remain live. Do not deliberately require a live conforming implementation to deadlock in order to “match” a weakened model.

Q05 supplies an explicit exception to that regression disposition: the forgotten-rejection trace remains a known limitation to characterize after relevant history is discarded. Preserve the stronger model oracle and report any reproduced resurrection; do not delete it or count the permitted gap as a conformance pass. The exception does not cover memory corruption, over-admission, or failure to honor still-retained object/operation state.

## Coverage gaps and additional cases

“Not formally covered” differs from “missing in prose,” and neither automatically means the protocol is defective.

| Gap | Evidence boundary and required addition |
|---|---|
| Accepted Q05 limitation: obsolete construction after discarded history | Analyze resource exhaustion before/after acceptance, peripheral contention, dynamic neighbour bindings, wrong/duplicate data and conditional O detection/removal. Existing model proof assumes rejection survives retirement; this implementation makes no such general guarantee. Use ID05 and LIM characterization cases; full mitigation is deferred. |
| Distributed admission and more than two operations | VG coordinators reside on K0 and admission is abstract. Add multi-coordinator reservations, delayed authorization, three-way conflict/fence races and partial initial fan-out. CON/REQ tests and a concrete admission refinement. |
| Rich Boolean dependencies combined with later graph operations | VN has up to three channels and richer P; VG's combined model has a required or optional single-channel addition. Add optional expiry after parent success concurrent with removal, rich P with later removals, and shared-host capacity. PRED/CON/MGMT tests. |
| Truthful startup and shared-memory closure | Abstract STARTED/CLEANED do not verify Q12's startup-marker convention/queue path, callback ownership, buffer lifetime, guest initialization effects or allocator. F10–F12, LIFE and RES tests apply to every replacement. |
| Parser and identity implementation | Models assume valid immutable authenticated requests and abstract old-traffic isolation. Q08 explicitly defers cryptographic authentication, leaving authentic origins as an evaluation assumption; cryptographic anti-spoofing tests are future work. Retain malformed/truncated/oversized-message tests, replay, logical wrong-owner/port/instance checks, endian/width issues and counter exhaustion. For changed-body duplicate submissions, test O's immutable association and the Q08 receiver-discard policy, recording its departure from M1 rather than claiming changed-body detection. REQ/ID tests. |
| Result query/retirement and bounded storage | Prose recognizes the obligation; executable models do not specify a production policy. Add O outage pressure, result serialization pressure, retired queries, retention with unresolved optional work, and delayed traffic after full-record reclamation. MGMT/RES tests. |
| Additional boundary cases identified in this review | Strict processing-time expiry at equality; checked clock/revision arithmetic and admission bounds; stale callback after slot reuse; required completion handling at saturation; authorized-but-undispatched edges in removal incidence; optional fan-out starving a sufficient subset; fresh-request deterministic encoding and retained-opId discard without body comparison; report loss during shared cleanup; artifact staging exhaustion. Apply the settled Q07–Q18 oracles, marking only genuinely unresolved R entries as decision-dependent. |
| Application service and security boundaries | Q20 selects the minimum CO2/local-control path, optional occupancy and QoC-based service assessment. Concrete QoC metric/thresholds, fallback and command outcomes remain open. Q21 selects a simple single-room plant with simulated peripherals; measurement coverage and suitability qualification remain pending. Q19 defers synchronization and acquisition-age guarantees; any later age requirement needs clock/provenance qualification. Guest import/resource isolation is not enforced by simulator resource declarations. Add application and capability tests, without claiming Byzantine KaOS tolerance or actuator correctness from protocol success. |
| Environment fidelity | Test control-plane loss, duplication, deterministic phase races, multi-host clock behavior, resource caps, and shared-core host/embedded parity. Radio, fragmentation, bandwidth contention and hard real-time behavior need additional environments if those claims are required. |
| Restart, migration and recovery | Explicitly excluded by the verified scope. Record as future specification work under Q02/Q22, not as defects to patch into the six operations implicitly. |

Completion means the six operations meet the selected requirements with Q05/Q08 deviations and Q12's trusted-marker convention explicit, every mandatory test passes within that scope, the final Q14 model checks and Q01 measurements are supplied, and application claims use the selected R07/R08 criteria. Classify deferred, blocked and known-limitation cases separately; a disposition alone is not a passing required test. Report post-retirement replay risks and authentication/duplicate-policy limits alongside results; do not claim full conformance to the stronger reference protocol. The current finite checks and planning audit do not establish implementation completion today.

### 2026-10-20: OBS02 entry-barrier verification increment

Selected the first actionable observation/fault work from the work register,
now integrated under [agreed work](#agreed-work-that-can-proceed).
Compared a manual runtime adapter harness with real WAMR entry guards and process
report polling. Use existing `core_wamr_attach_entry_guard`: it establishes an
actual worker boundary outside registry/mailbox locks without production API or
storage changes. Manual adapters would provide weaker runtime evidence; report
polling would not itself stop execution at a boundary.

New `verification/graph_core/tests/lifecycle_barrier_test.c` covers before-configure
and before-main gates with no fault, authorized no-edge removal, and exact
controller-deadline expiry. An unrelated actual worker reaches accepted INIT
while the target is held. Cancellation and retained charges are observed before
release; denied entry, disposal, CLEANED and reclamation follow release. Positive
startup controls prevent vacuous pass. Every case runs twice and compares an
eight-event imposed partial order; this is not complete schedule replay. Gate
waiting has a ten-second monotonic bound; controller observation loops have
five-second bounds; CTest has a thirty-second outer limit.

Default ASan/UBSan suite passed 51/51; four-node suite passed 25/25. All 87
pre-existing C/H/Python verification sources in the recorded baseline are
unchanged; `graph_core/` has no changes. Independent spec and standards reviews
found no blockers. Simplified a redundant gate flag and bounded the worker wait
following review. Evidence: `verification/graph_core/barrier_verification.json`,
with hashes, full logs and event ledgers under `results/barriers/`. Historical
continuation results remain separate.

OBS02 remains partial: preparation and cleanup are observed outcomes rather than
named fault gates. Expiry advances controller time and does not independently
verify WAMR real-clock expiry. Runtime flags prove entry publication, not a guest
instruction trace. This channel-free local fixture does not cover collector
crashes, UDP observation loss, full replay, hardware qualification or all OBS03/
OBS05 obligations. Next observation work is listed under [agreed work](#agreed-work-that-can-proceed).

### 2026-10-20: OBS02 adapter preparation/disposal completion gates

Continue the observation work with a separate core-only fixture:
`verification/graph_core/tests/runtime_phase_barrier_test.c`. Compared real WAMR
internal hooks against a manually scheduled `core_node_runtime` adapter. Existing
WAMR hooks cover entry only; adding internal test gates would enlarge production
interfaces. Choose the adapter contract for this increment and leave actual
WAMR preparation/destruction and process barrier qualification open.

Six cases combine no preparation fault/removal/exact addition expiry with
cleanup release before cutoff or after exact cutoff. Artifact handoff is real;
the adapter delays private arena allocation and preparation publication while
another slot starts through real node-service INIT acceptance. Late preparation
after cancellation cannot authorize dispatch. During disposal hold, execution
has stopped but storage and worker ownership remain; just-before/equal-deadline
outcomes are pending/unresolved. Freeing storage precedes disposed publication;
only then can the core reclaim and report CLEANED. Eventual removal succeeds,
failed addition becomes failed-clean, and historical addition success remains.
An extra core step checks against repeated reclamation; the unrelated node is
independently removed and total reservations reach zero.

Spec and standards reviews found no blockers. Added assertions for the retained
worker execution/action and removed ambiguous fault wording for the positive
control. This is serialized adapter-contract evidence, not actual worker
concurrency, WAMR internal allocation/destruction, preparation failure, channels,
collector loss or full replay. The unrelated slot starts during preparation
hold and is checked for continued liveness during disposal hold. Build/test
results and preserved test-source baseline are in
`verification/graph_core/adapter_barrier_verification.json`; old evidence stays
historical. No production code or pre-existing test sources changed.

Final validation for this adapter increment: ASan/UBSan default52/52 and
four-node26/26 passed. All88 pre-existing test/fixture sources match their
baseline. Initial focused failure exposed a fixture pool-budget mismatch
(4096-byte generic pool versus128-byte expectation); corrected only the new
fixture setup. No runtime defect was found in this slice.

### 2026-10-20: OBS03 post-capture observation corruption

Added a separate `graph_observation_loss` case in both host profiles using the
existing real-host feedback workload and unchanged sample/timing evaluators.
Sixteen copies (eight faults per device) remove first/middle/last/all records,
remove a terminal summary, change its drop counter, duplicate a record or reorder
two records. Every damaged copy must produce inconclusive sample and timing
assessments. The untouched trace has successful sample evidence and successful
operation reports; complete timing may pass or fail its budget. No claim of
absence or cleanup is inferred from the damaged copy.

The same new fixture supplies every proper prefix of one synthetic authorized
snapshot to the collector adapter. All eleven prefixes are rejected without
retention; a complete positive snapshot stores and receives the exact ACK; a
later malformed duplicate cannot erase it. No collector implementation change
was needed. Spec review found no blockers. Standards review caught fixed output
directory reuse; corrected to unique retained run directories and reran against
the same populated root successfully. Subsequent review found no blockers.

Scope stays partial OBS03: synthetic drop-counter alteration is not live overflow
or counter production; direct collector input is not network truncation. Kernel
loss, relay overflow, injector lateness, collector crashes and full replay remain
open. Retain healthy traces, per-copy assessments, suite logs and hashes in
`verification/graph_core/observation_loss_verification.json` and
`verification/graph_core/results/observation_loss/`. Production code and existing
test sources remain unchanged.

Final OBS03 slice validation: default53/53 and four-node27/27 ASan/UBSan suites
passed. All89 prior C/H/Python test/fixture sources match the recorded baseline.
Each profile retained the original successful run plus16 inconclusive corruption
assessments and11 rejected snapshot prefixes; complete snapshot control passed.

### 2026-10-21: MGMT14 collector crash boundaries

Added `collector_crash_test.py` and its CTest registration in both host profiles.
Compared channel-free runtime evidence with a two-host remote-channel topology;
selected the latter to assert O retains nonempty possible incidence after loss.
A spawned process owns the actual GraphCollector/GraphTransport RAM archive.
The parent keeps O alive but deliberately delays querying/applying the removal
result. The child alone obtains and stores a validated complete node/channel
removal snapshot, then signals a pipe boundary without transferring the payload.

Case one SIGKILLs after storage and before storage ACK. A later raw query returns
the same snapshot hash; O retains incidence until it deliberately applies that
report. Case two first requires CHANGED for the exact storage ACK, then SIGKILLs.
The later O query returns NO_RETAINED_RESULT with empty current observations;
O's possible incidence remains. Both assert collector exit -SIGKILL, successful
peer node evidence, and a fresh addition/removal with the collector still dead.
No sleep selects the crash phase: storage and ACK completion are observed before
kill. Polling and process waits have finite failure bounds.

This characterizes volatile DB evidence loss under MGMT14 with delayed O
observation (MGMT13), not an O outage, archive durability/restart recovery,
device crash or unrelated-channel data isolation. Original result bytes stay in
the collector; observer metadata cannot be fed back to O as closure. Existing
P4/P5 helper methods are reused without editing them and only new test methods
are selected. Spec and standards reviews found no blockers; clarified the log
field separating post-crash incidence before retrieval from final authority
closure. New per-invocation output directories preserve repeated-run evidence.

Evidence: `verification/graph_core/collector_crash_verification.json` and
`results/collector_crash/`. Production sources and all prior test sources remain
unchanged. Full suite results are recorded below when available.

Final collector increment validation: default54/54 and four-node28/28 ASan/UBSan
suites passed; both new SIGKILL cases passed in each profile. All90 prior C/H/
Python test/fixture sources match the recorded baseline. No production defect
was found and no production interface/storage change was needed.

### 2026-10-21: OBS03 live relay overflow rejects run completion

The next observation slice found a concrete gap: GraphRelay already recorded
actual queue overflow, but GraphRun could exit normally and its callers could
write a normal passing result. Added a small counter to the existing GraphRun
log path (sibling `sim_env/sim_env/graph_run.py`). Count only `packet_dropped`
with `reason=relay_overflow`, once per packet copy, under the existing log lock.
After relay shutdown/join, append an error if nonzero and include the count in
the existing teardown record. Existing context-manager failure handling prevents
normal result writers from proceeding. Protocol evidence, O state and complete
application trace semantics remain independent of this run-validity failure.

Compared a shared offline assessor/all-evaluator changes, a new relay assessment
API, and central GraphRun rejection. Chose the last: fewer interfaces and no
conflation of complete channel observations with an invalid fault experiment.
Standalone offline sample/timing evaluators still assess recorded application
evidence; callers must also inspect run teardown errors for injector validity.

The new `relay_overflow_test.py` uses a real host and real UDP relay, with the
existing relay factory seam selecting capacity one. A positive query round trip
precedes one delayed duplicated query. The original fills the queue, copy1
positively reports overflow, and a bounded condition wait observes that event.
The query timeout is caught inside a successful context body; independently,
context exit must reject overflow. Its original gets run_ended at shutdown,
which is not counted as overflow. A requested-loss control drops one packet but
exits normally with zero overflow count. Original test first failed because no
RuntimeError was raised (and no count existed), then passed after the fix.

Spec and standards reviews found no blockers. Ruff formatting and lint passed.
No graph_core production change or existing test-source modification. This
covers a finite queue path, not default256-slot throughput, active graph data,
kernel receive losses, injector lateness, device timing or full replay. Evidence
and hashes are in `verification/graph_core/relay_overflow_verification.json` and
`results/relay_overflow/`; the original simulator file is preserved with that
record so the precise change is reviewable despite the pre-existing dirty tree.

Final live-overflow validation: default55/55 and four-node29/29 ASan/UBSan suites
passed, including sibling Python regression discovery in the default suite.
Ruff lint/format passed for the changed simulator module and new test. All91
pre-existing C/H/Python test/fixture sources match the captured baseline.

### 2026-10-21: focused shared-host crash-stop (HOST03)

Selected the next settled observation/fault item after live relay overflow.
Compared a test-only GraphRun subclass that exempts killed processes with one
public `GraphRun.crash(device)` operation. Chose the public operation: reusable
explicit fault injection with one argument, no restart/freeze/recovery policy,
and no graph-core production changes. A per-device management lock plus a
process-state lock serializes kill/wait/classification with concurrent health
checks. Require a live managed process, record intent, send SIGKILL, confirm
exit within five seconds, then record completion. Only that confirmed exit is
waived in health checks and teardown. Unknown or already-exited hosts reject;
unrequested process death, stderr, reader failures and overflow remain errors.

The new `host_crash_test.py` runs two actual WAMR hosts. Device1 hosts a source;
device2 hosts two distinct sinks with required AND channels. Both sinks must
positively produce GET observations before the crash. Device2's process death
stops its node/endpoint/coordinator roles together. Queries to both failed
coordinators time out. Source removal on the survivor reaches Unresolved: local
node/endpoints CLEANED, remote endpoints not CLEANED. O's two possible incidence
entries remain unchanged through teardown. Negative control directly kills an
unrequested host and requires both health-check and context-exit rejection.
New tests cover unknown/repeated crash calls without modifying old tests.

Before implementation, the shared-host test failed on missing `crash`; the
unexpected-exit control passed. Both then passed against the implementation.
Separate spec/standards reviews found no blockers. Concurrent crash versus
survivor management is inspected, not separately exercised by this fixture.
Ruff requested combining nested context managers in the new negative control;
that formatting-only simplification was applied and lint passes.

Scope remains focused HOST03: no unrelated U/channel Z isolation, all-lifecycle
fault schedules, complete sample/timing trace, freeze/resume, restart/recovery
or hardware qualification. SIGKILL need not produce CHANNEL_END; buffered
stdout can appear after crash completion, so log arrival order is not a causal
execution barrier. Process death never supplies graph closure evidence.
Evidence, original simulator snapshot and exact diff are retained under
`verification/graph_core/results/host_crash/` and the verification manifest.

Final HOST03 validation: default56/56 and four-node30/30 ASan/UBSan suites pass,
including default sibling Python regression discovery. The two new cases pass
again in each profile after the Ruff syntax-only correction. All92 previous
C/H/Python test/fixture sources retain their captured hashes. No additional
embedded resource, timing or target qualification is claimed.

### 2026-10-21: OBS02 real WAMR preparation/destruction gates

Continued the settled lifecycle observation slice with a new host-only fixture,
`verification/graph_core/tests/wamr_phase_barrier_test.c`. No graph production
code or existing C/H/Python test/fixture source was changed. The shared workload
build includes a new CMake registration; it reuses the unchanged restricted node
guest and links the unchanged production runtime and engine.

Two independent design explorations compared production callbacks, including
private source in a test translation unit, existing entry/notification callbacks,
and link-time wrappers. Selected wrappers: no firmware interface, task, storage
or conditional production build. Existing notification callbacks must remain
bounded/nonblocking and cannot implement a held gate. The Linux qualification
profile supports GNU ld `--wrap`; these fixtures are not target firmware.

Chosen gates: after successful real `wasm_runtime_instantiate`, before KaOS
assigns/publishes its instance; and before real `wasm_runtime_deinstantiate`,
after execution-environment destruction but before instance/module/arena/artifact
release. Submit the target alone, observe its first gate, then start the other
worker. Match subsequent destruction by captured instance/module handles.
Wrappers hold no registry/mailbox lock; all waits are bounded and monotonic.

Cross no-fault/removal/exact addition expiry with immediate/after-cutoff disposal
release. Positive startup controls must succeed. Fault schedules require zero
target entry-guard attempts and no commit/dispatch/start/success history. At
cleanup deadline-1 the closing operation remains Pending; at equality it becomes
Unresolved with both charges, worker ticket and reservation retained. The other
node must still be running and uncancelled before its independent removal, which
must finish while target destruction remains held. This establishes fresh worker
progress at both boundaries. Actual deinstantiate/unload completion is checked
when CLEANED is observed, before service shutdown. The unload wrapper rejects
premature disposed publication. Final charges/reservations reach zero; a repeated
service pass cannot reclaim twice. Each of six schedules runs twice and its
bounded ledger must follow the expected order and match its repeat (56 records).

Review strengthened the original fixture with unrelated-liveness checks before
removal, explicit expected ledger order, and destruction-order assertions before
shutdown. No production defect was found. The implementation skills' incremental
verification/review guidance fits; ticket-driven integration branches, automatic
commits and generic abstraction suggestions were not adopted for this local
verification slice. Fixed-storage embedded ownership remains the controlling
design constraint.

This is partial OBS02 qualification, not complete Q14 or P9 acceptance. Exact
controller-time injection does not qualify WAMR wall-clock expiry. No allocation
failure, network/collector interaction, arbitrary intra-WAMR instruction barrier,
full process replay or physical target timing is covered. Evidence and baseline
hashes are retained in `verification/graph_core/results/wamr_phase_barriers/` and
`verification/graph_core/wamr_phase_barrier_verification.json`.

Final validation: default 57/57 (63.04 s) and four-node 31/31 (47.01 s) ASan/UBSan
CTest entries pass, including twelve new schedule executions in each profile.
All 95 pre-existing test/fixture source hashes and all 47 production-file hashes
match the baseline. General Python discovery retains 40 opt-in skips; the
default three-node pipeline also remains skipped. The new fixture has no skips.
The initial build required sandbox escalation for WAMR's generated version
header in the sibling dependency tree; a sandboxed focused run hit LeakSanitizer's
ptrace restriction. Authorized runs outside that sandbox retain leak checking
and pass. No semantic implementation failure or existing-test change occurred.

### 2026-10-21: Two-port transport scheduling

Implemented the agreed P8/R04 transport increment. `core_host_platform` separates
readiness from single-datagram nonblocking receive by class; send selects the
class. Shared UDP code owns two sockets, class-specific peer attribution and
source ports, partial-startup cleanup and the existing wake descriptor. Remote
devices require both addresses; authority/collector remain management-only.
POSIX arguments/readiness and ESP Kconfig change together; old single-port
canonical-host launch commands are incompatible. Frozen P2 stays unchanged.

One control task performs initial lifecycle decisions, application RX4,
management RX1, staging, bounded application TX via the existing endpoint cursor,
management replies plus a full finite peer pass, one eligible artifact attempt,
and maintenance. Due timers are checked between inputs and before application
sends. Accepted input gets paired pre/post lifecycle passes under the registry
lock. Unknown, malformed and oversized datagrams consume receive budget; an
exhausted batch requests an immediate next round without a new wake. Application
submission failures remain dequeue-and-drop. Fixed saturating counters expose
receipts, rejects, submissions, artifact attempts and local timing maxima.

Chosen simplifications: retain existing endpoint cursor and task priorities;
complete each finite management peer pass instead of adding a second cursor;
reuse bounded reply storage directly; one artifact-attempt timestamp persists
across blocks, failures, retries, restarts and new transfers. Production spacing
is a provisional 1000 us, configurable through `artifact_interval_us`; test
adapters explicitly select zero where their old 100-us deadlines require it.
Terminal timeouts bypass pacing and all original deadlines remain authoritative.
No per-endpoint task, new application retry queue, framing version or CRC change.

Simulator routes are `addresses[owner][class]`, with class keys `management` and
`application`. O has only management. Both classes retain the same directed-link
policy/RNG and one global pending-copy heap/limit. Observations add `socket_class`
while retaining `plane`; GraphRun records both routes and updates canonical-host
launch/readiness. Three simulator fixtures and six C/local process fixture files
were adapted under the user's explicit single-port-conflict approval. Existing
behavioral assertions/deadlines remain. Collector/P7 device-ID argument positions
needed correction after the first full run. Collector formatting preserves its
AST. No other baseline test sources change.

Ten new fake-clock scenarios cover budget exhaustion, malformed traffic,
wrong-class input, bounded recurring service, continuation without wake,
round-robin persistence for a non-dividing endpoint count, failed-send dequeue,
removal/expiry before data, block/retry/terminal deadline behavior and failed-send/
restart pacing. Real UDP checks extend existing routing/cleanup coverage.
Final suites pass 58/58 default and 32/32 four-node under ASan/UBSan/LSan. Python
lint/format pass on all ten changed Python files; 9 opt-in runtime process cases
also pass. Default Python discovery has 171 entries and 41 expected opt-in skips;
one three-node workload is skipped only in the smaller capacity profile.

The retained `transport_contention.py` uses identical 64-byte feedback guests and
16 KiB custom-section-padded artifacts for preserved single-port and new two-port
host/runner pairs. All four captures have complete traces, successful node
addition/removal and feedback GETs during the actual 64-block transfer. The new
hosts record 63/default and 68/four GET copies during transfer. One run per
profile/version is exploratory evidence, not latency acceptance or proof of a
performance improvement. Exact p50/p95/max, transfer windows, counters and
provenance are retained. The standalone unchanged CRC probe covers 256 B through
64 KiB; median at 16 KiB is 0.335 ms, at 64 KiB 1.318 ms in this unsanitized -O2
host run. Sanitized final-artifact-handler maxima of 1.454/1.738 ms include CRC,
parsing and copying under the registry lock. Full lock-wait/hold instrumentation
and isolated final-block correlation remain qualification work. No optimisation
is justified against a declared application target yet.

ESP-IDF v5.5 network-host cross-build passes with four-node profile, -Og and
12288-byte main stack; binary is 917712 bytes. A same-command -fstack-usage probe
reports 1232-byte frames for step and receive_batch; this excludes callees and
is not a high-water measurement. No board was available for Wi-Fi execution.
Continuous ready work can keep priority-3 control runnable ahead of priority-2
workers. Target worker progress, stack/heap/network bounds, timer/lock timing,
combined saturation, admitted workload targets and final composed Q14 checking
remain open. These functional results do not close R05/P9.

Evidence: `verification/graph_core/transport_scheduling_verification.json`,
`results/transport_scheduling/`, and `guide/verification.md#two-port-transport-scheduling`.

### 2026-10-22: Graph core simplification, steps 0–6

Implemented the behavior-preserving portion of `graph_core_simplification_plan.md`.
Shared UDP now sits beside protocol sources, with its header in `include/` and
the socket build boundary preserved. Artifact option validation reuses CoAP's
existing helper; control routing finds URI-Path once; host dispatch passes its
borrowed decoded message into control. Raw callers retain their original entrypoint.
Channel comparisons and descriptor encoding have one implementation each.
Admission uses two read-only scope checks with publication/capacity visible in
the main function; a larger temporary-plan alternative was compared and rejected.
Named request modes replace three Boolean mode arguments. Endpoint closure,
node handoff eligibility, host phases and WAMR teardown now expose ownership and
ordering without new state, tasks, allocations or policies. Initial node cleanup
and exactly two edge-then-node passes, clock/lock placement, original deadlines,
receiver-first activation and partial-init/shutdown ordering remain intact.

All 87 captured existing C/H/Python verification files are unchanged. Two new
tests cover raw/decoded control equivalence and worker-startup unwind. Full
sanitized suites pass 60/60 default (62.59 s), 34/34 four-node (45.58 s), and
9/9 opt-in runtime process cases (1.069 s). Discovery retains 41 opt-in skips;
the default local three-node pipeline remains skipped. Each profile also passes
179,139 generated request/admission and 1,456 UDP-parser comparisons against
the dirty-tree baseline. Protocol-only build/check and ESP-IDF v5.5 four-node
network-host cross-build pass; ESP binary is 917856 bytes. No board run occurred.

Eight fixed record types retain their sizes on native and ESP32 compilers in
both profiles. Xtensa `-Og` step/receive frames remain 1232 bytes; the host's
control-handler frame decreases by 224 bytes. Some extracted calls increase
local stack use, including the retained raw control entrypoint's 64-byte increase
in wrapper-plus-handler frames. All reports are recorded; these are not peak
call-chain or physical high-water measurements and do not close target limits.
Existing stack/capacity settings are unchanged. Sandbox failures were socket/
LeakSanitizer restrictions, and WAMR configuration needed its generated sibling
dependency header; authorized reruns passed. Implementation-skill tracker/commit
workflows and codebase-design test-deletion advice were excluded because they
conflict with this task's local scope or explicit test-preservation instruction.

Application-CoAP slice P remains pending. The user clarified that maximum remote
rate is to be measured, while required rate likely belongs in the manifest whose
interface is still being designed. No rate cap or manifest fields were invented.
MID reuse, response suppression and congestion behavior need that contract;
existing host/simulator wire fixtures need specific approval before migration.
Evidence: `verification/graph_core/simplification_verification.json`,
`results/simplification/`, and `guide/verification.md#simplification-refactor-verification`.
