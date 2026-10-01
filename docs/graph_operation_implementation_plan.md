# Graph operation implementation plan

This plan designs the six graph operations from the thesis protocols and finite verification for a small embedded device, with evaluation in `../sim_env`. Existing feature implementations may be replaced completely. The proposed starting point is a new bounded protocol core, explicit runtime and endpoint ownership, and shared host/embedded interfaces. Reuse is justified by protocol fit, measurable resource cost and testability; existing code structure is not a constraint on the design.

Prepared on 1 October 2026 and revised to incorporate the user's permission for complete implementation replacement. Read the [design questions](graph_operation_design_questions.md) first; technical choices below remain proposals where those questions are open. The [test catalogue](graph_operation_test_plan.md) defines concrete stimuli and expected observations independently of legacy compatibility. The [verification inventory](graph_operation_verification_inventory.csv) maps every existing configuration to planned implementation regressions and records its audit scope. These documents do not change runtime code.

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
| O authorizes graph changes; Kc coordinates one operation; each Ki owns its local objects. | Local services may be separate tasks, but remain one protocol entity per host. An application observation/request does not grant remote authority. | SH §1; FT responsibilities; KD interactions |
| Logical node, concrete instance and physical device are different identities. | Keep binary/manifest identity, instance ownership, and placement separate. Resources and runtime history belong to the instance. | KD system model; V2 §2 |
| `channelId = (sourceLogicalNodeId, destinationLogicalNodeId, serviceId)` is directed and globally distinguished by the full tuple. | Multiple services on one node pair and one service on disjoint pairs are valid. Instance/port/placement are descriptors, not a replacement for channel identity. | V2 §2; SH §2 |
| One preparing/active/removing binding occupies a channel identity. | Completed closure and old-traffic isolation precede reuse; unresolved work retains ownership. Fresh removal identity must target the original creation binding. | M1/M6, E3/E5 |
| P is a fixed monotone dependency predicate over complete channels. | Same predicate at commit and startup; validate `P(Channels)`. Failure does not change declared group totals; no minimum-cardinality optimization is required. | V2 §3; VN BooleanContracts |
| Node and endpoint preparation are independent. | Reserve registry/ports before guest execution; installing inactive associations must not require running N. Preserve mappings during runtime initialization. | M3; IN |
| Commit grants no execution or data permission. | Only receiver activation opens Receive/Get; accepted receiver ACTIVE permits sender activation and Put/Send. Local channels obey the same ordering. | M3–M5 |
| Dispatch, execution, STARTED emission and acceptance are distinct. | Recheck guards at execution; start at most once. Missing confirmation does not prove that no application effects occurred. | M5; V2 F9–F12 |
| Startup success is historical and terminal. | No O or peer outcome acknowledgement, all-channel join, first-output requirement, or ongoing-health guarantee. Pending optional work retains owners/deadlines after success. | V2 §§5,7–9; SH M7 |
| Cleanup includes every selected object, even never-ready objects. | Failed node addition cleans N and both endpoints of every declared channel. Node removal uses complete current incidence; edge cleanup preserves existing runtimes. | M1/M6 |
| CLEANED means terminal cancellation, disabled access, safe lifetime, and released owned resources/associations. | Stopping a thread or deleting a queue alone is insufficient. Selection at Kc does not immediately revoke remote permission. | M6; IN; VG registry counterexample |
| Operation outcomes are Pending, Succeeded, FailedClean and Unresolved. | Rejection precedes acceptance and is not a fifth accepted-operation outcome. Incomplete requested removal remains Pending/Unresolved, never FailedClean. | M1/M7 |
| Commit and dispatch history are monotone facts. | Retain `commitRecorded` and `startDispatched` or an equivalent representation; derive Abort, CommitNoStart or StartIssued. Do not independently mutate a redundant failure-history state. | VN walkthrough; SH M7 |
| Deadlines belong to local monotonic clocks, with fixed start events. | Retrying, querying, commit and startup never reset allowances. Ki cannot read Kc time and does not silently expire prepared reservations. | M7, A9/P5 |
| Capacity includes partial allocations, outstanding actions and retained results. | During a prolonged orchestrator outage, retained results and unresolved obligations may exhaust the device’s reserved storage, preventing it from accepting further operations. Detailed result retirement and old-command rejection are separate policies. | A8/E6; SH M7; IN |
| Endpoint lifecycle, communication health and application service are separate observations. | Put acceptance, transport receipt, Get, application acceptance and actuator execution are different completion boundaries. | V2 §8; KD contracts; FT |
| WASM tasks use authorized ports through KaOS; peripheral placement constraints matter. | Preserve instance-scoped authority and hardware requirements. Spare compute cannot substitute for a missing physical sensor or actuator. | KD/FT |
| The selected application is room ventilation. | Model sensing, validation/aggregation, supervisory policy and a device-local feedback/actuation loop. Movable computation and hardware-bound tasks are distinct; this does not implement migration. | FT representative application |

### Older statements requiring reconciliation

| Older wording | Treatment in this plan |
|---|---|
| `Unbound/Bound/Paused`, receiver HELLO handshake, and channel generation in KD/OTHER | Map current construction to Prepared/Active and explicit role permissions. An optional transport handshake may implement interface validation; it cannot replace M4 or create a new universal protocol exchange. Incarnation isolation is required; a generation field remains a design choice. |
| Possible application readiness response in KD | Preserve dynamic-interface validation and any platform-specific binding hook, but M3 expressly adds no universal application READY handshake. |
| O feasibility/reservation milestones and old peer outcome acknowledgements | O may keep observations, but Kc decides from attributed evidence; no extra success-finalization phase. |
| Atomic insertion, migration, replacement and pause/resume examples | V2 does not promise atomic graph visibility or in-place rebinding. Migration/state transfer and autonomous recovery need separate specifications. |
| V2 §11 suggests querying current state as an alternative to retained reports | SH M7 explicitly requires retained attributed outcomes and bounded admission. Current-state queries cannot reconstruct historical commit/start/cleanup evidence. Keep queries and define retention policy. |
| Threshold shorthand in V2 versus Boolean-only revised verification | Q10 selects public support. Shorthand needs new parser/arithmetic/equivalence coverage; existing Boolean checks do not verify it. |
| Main thesis draft uses 4-second sampling/12-second age examples | FT explicitly says numerical ventilation limits have not been selected. Treat draft numbers and simulator defaults as illustrative, not approved requirements. |
| Derived files still say “not independently verified” | Report VG's newer finite evidence with its actual bounds; do not silently promote that evidence to an implementation or unbounded proof. |

## Operation completion and failure scope

| Operation and coordinator | Acceptance and construction | Success evidence | Failure or removal obligations |
|---|---|---|---|
| Node addition with channels; N host | Fixed manifest, valid P, instance/incident reservations; independently prepare runtime and endpoints, commit, activate sufficient channels, dispatch once. | Accepted STARTED for N with P(ActiveSet), local usability and unexpired node deadline. | N plus both endpoints of every declared channel. Channel failure alone cleans its pair; before dispatch, continue if P remains possible; during Starting, known loss of P(ActiveSet) selects whole cleanup. |
| Node addition without edges; N host | Established empty incidence, empty manifest, P=true; prohibit incident admission until success or confirmed cleanup. Prepare/commit/dispatch still apply. | Accepted STARTED; empty channelStatus. | N, including cancellation against a delayed creation. No endpoint or channel deadline. |
| Edge addition; source host | Both node additions have succeeded and support dynamic binding; reserve fresh binding, prepare endpoint pair, commit, activate receiver then sender. | Accepted ACTIVE for both endpoints before channel deadline. No node STARTED gate. | Both endpoints, including never-ready peer; keep both runtimes/unrelated channels. Abort/CommitNoStart history applies. |
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
| ENV03 | [Container manager](../container_mgr.c) `init_module` runs guest `setup`, then monitor startup proceeds to `main_app`; `execute_function` sets RUNNING before the WASM call. [Signals](../kaos_signals.c) maps guest liveness signal 0 to EVENT_CONTAINER_LIVE, used for LOAD completion. | Design a runtime lifecycle interface with inert preparation, guarded entry and runtime-attributed STARTED. Replace the manager, entrypoint ABI and signal-based completion path where necessary. |
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
| ENV14 | Queue destruction in the host port requires no concurrent users; timers cannot delete themselves inside callbacks. Core uses pointers, mutexes and variable-sized allocations. | Make cancellation and quiescence explicit across port APIs; test callbacks and queue consumers still in flight at removal. |

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

Simulator example modules request 65,536-byte runtime stack and heap each, while their compilation uses a separate 16 KiB linear stack. These settings describe different allocations and must be measured rather than summed blindly. Even two 64 KiB per-instance reservations would be 128 KiB before module/runtime/queue/native-stack overhead; they are not suitable universal defaults for an unspecified embedded board.

### Evidence from the initial planning audit

The following checks describe the inspected implementation before replacement. They were performed when the initial plan was prepared, not rerun for this documentation revision. They provide a reference and do not impose compatibility gates on new code.

- VG read-only `run_checks.py --audit`: **55/55** current input and retained-log matches. Its reported results comprise 29 positive checks, 16 expected negative counterexamples and 10 witnesses.
- VN independent hash comparison: **35/35** current model/dependency/configuration hashes match saved summaries; all entries say expected result matched (19 positive, 7 negative, 9 witnesses). This older summary does not hash protocol text, runner or logs; it is a narrower audit than VG.
- Simulator `.venv/bin/python -B -m unittest discover -s tests -v`: **41 discovered, 21 passed, 20 process/socket integration tests skipped**. Native `build/port-test`: exit 0.
- Existing build-manifest hashes: **4 artifacts, 16 host inputs and 33 copied-core inputs match**. `patch --dry-run --batch --fuzz=0` applies the current host patch to this core without conflict.
- `uv`, CMake, C compiler, patch, Java, `/opt/wasi-sdk/bin/clang`, and an ESP-IDF export script are present. Presence is not proof of an embedded build or hardware readiness.

No TLC exploration, native rebuild, process/socket integration run, sanitizer run, or embedded execution was performed for this documentation task. Existing dirty Thesis/simulator work was preserved. Subsequent simulator builds/runs must use a writable output/build location and permit local sockets/process signals; those environment requirements are documented in SIM.

## Proposed replacement architecture

Design a platform-independent protocol core with typed events, explicit owned records and effect interfaces. A proposed transition function consumes a validated event and a local time observation, updates the relevant operation/object records at a consistent decision boundary, and issues bounded work requests. Runtime work, endpoint preparation, transport delivery and timer handling remain separately observable actions. This does not make an entire operation atomic or require a single worker for all device activity.

Host tests can control these effect interfaces deterministically; the process simulator and embedded port must execute the same production transitions. Runtime/endpoint workers complete through attributed events, while data access enforces endpoint permission and lifetime through a bounded API. Choose whether data operations use locks, ownership transfer or another mechanism under Q13/Q14; routing every payload through one management event queue is not required.

The following decomposition is a proposal under Q03–Q17/Q23. Component boundaries are responsibilities, not mandatory files, threads or retained legacy functions.

| Component | Contract and replacement boundary |
|---|---|
| Request and contract decoder | Versioned bounded parsing, authorization, immutable request comparison, descriptor/P validation and safe arithmetic. New schema and selected codec independent of Beacon framing; reuse a codec library only after measuring its bounds. |
| Operation engine | Per-op phase, immutable request reference, commit/dispatch facts, accepted observations, fixed scope, deadlines and results. Implement all six operations as specializations of M1–M7; the legacy monitor and its operation hierarchy may be replaced. |
| Admission and binding registry | Atomic local resource/result reservation, instance fences, complete incidence linked to O reservations, exclusive binding identity and safe internal handles. Define new storage around these invariants rather than extending the old module registry by default. |
| Runtime service | Reserve instance/ports, instantiate inertly, validate declared entrypoints, perform guarded single dispatch/execution, emit truthful STARTED, quiesce and release before CLEANED. The engine and ABI are selected against this contract; the existing container manager is optional. |
| Endpoint service | Distinct role lifecycles, inactive port installation, activation permissions, evidence and scoped terminal cleanup. Choose buffer/queue storage from channel contracts and lifetime requirements; replace the current lazy queue paths. |
| Identity and evidence service | Validate op/request/creation/instance/endpoint/owner/issued-action matches, compare stream revisions, reject obsolete local and remote completions; isolate delayed data and handles. |
| Resource and lifetime service | Bounded pools/quotas, allocation ledger including partial work, outstanding access accounting, control/cleanup reserve, deterministic failure hooks and high-water measurements. |
| Deadline and scheduler service | Injectable monotonic time, fixed deadlines, bounded handlers, retries within original allowances and no retransmission after cutoff; fair progress for each admitted operation. Timer storage and worker layout are selected for the new resource profile. |
| Result service and management interface | Retained attributed results, queries by original opId, separate historic outcome/current channel status, bounded updates and selected retirement policy. A new simulator O client may use Python; operation conformance decisions remain in actual KaOS code. |
| Platform ports and build | Narrow clock, scheduling, transport, memory and runtime integration interfaces with host and embedded implementations. Build the same core directly for each; observation hooks must not change its protocol semantics. |

A proposed bounded record model separates `OperationRecord` (request, scope, history, evidence and deadlines), `InstanceRecord` (ports, capabilities, runtime handle and access lifetime), `BindingRecord` (channel and creation identities, placements, endpoint roles and observations), resource reservations, retained results and compact rejection records. These are logical ownership boundaries; share immutable descriptors or compact fields where it reduces measured memory without losing evidence. Capacity for continuing optional work must remain owned after the parent operation completes. Decide representation and field widths after Q01/Q03/Q05–Q07, not from existing structs.

Use the same message acceptance functions for local events and decoded remote messages. Do not enforce ordering only in the simulator. Keep a short consistent decision boundary around checking evidence/deadlines and recording commit/start/failure. Release locks before guest execution, network waits, allocation that can block, and user callbacks. Reserve any required completion-event capacity before issuing work so the implementation cannot lose the only completion or cleanup notification.

For each object, keep its creating authorization distinct from a later removal request. A shared owner cleanup task may fulfill multiple observers' obligations, but each accepted reply must establish closure of the exact object for the receiving request. Registry release may use a conservative endpoint-release-first order; it must not require a remote endpoint's memory to vanish before safe *local* storage can be freed. Full operation completion still requires all remote evidence.

## Embedded memory and processing plan

Choose Q01 limits before fixed layouts. Define separate bounds for `N` resident instances, `E` local endpoint roles, `O` in-flight operations, `R` retained full results, `T` compact rejection records, `Q` queued events, `B` queued payload bytes, artifact staging and expression complexity. A node-addition cleanup scope contains `1 + 2C` logical objects for C declared channels even if only some are local. A node's current incident scope may be larger than its original manifest and must have its own admission bound.

Build a measured memory ledger:

```text
peak managed RAM = fixed platform/control/runtime baseline
                 + instance records and actual per-instance WASM/native allocations
                 + endpoint records and uniquely owned buffers
                 + in-flight operation and full retained-result records
                 + compact rejection records and evidence indexes
                 + event/retry queues and artifact/decoder staging
                 + temporary overlap during preparation and cleanup
                 + reserved control/cleanup capacity and measured headroom
```

Count shared buffers once, while retaining separate endpoint obligations. Include allocator overhead, alignment, fragmentation, WASM linear memory, binary ownership/copies, TLS/native stacks, network buffers, timer structures, and result serialization workspace. Host `sizeof` and RSS are not 32-bit target RAM measurements. A detailed report may be much larger than its control record; bound both storage and response serialization. If persistence is selected later, budget flash layout, write amplification, wear and torn-write recovery separately.

Admission must reserve peak overlapping needs, not just the final steady state. Charge partial/failed/unresolved allocations until safely released; keep result slots charged after work capacity is freed. Optional channels must not exhaust resources required for a P-satisfying support set indefinitely (P3). Consider quotas or reservation priorities, subject to Q04/Q14; fixed unlimited fan-out is not acceptable.

Use preallocated pools or bounded allocators where measurements justify them; audit allocation in the selected WASM engine, codec and network paths before promising allocation-free operation. The implementation can eliminate duplicated legacy registries, compatibility layers, artifact copies and unnecessary native stacks. Compare these savings against the real cost of explicit evidence, lifetime records and rejection metadata; replacing code does not remove those protocol obligations. Avoid one native stack/timer per channel where bounded shared scheduling can meet progress. Represent qualifying sets as bounded bitsets and P as a bounded expression; evaluate it in work proportional to its encoded size. Do not enumerate every subset on the device to validate a positive Boolean grammar.

Measure event-handler time, lock hold time, startup/cleanup latency, timer lateness, queue occupancy, memory peaks and retained bytes during outage/churn. Derive deadline allowances from preparation, delivery/retry, dispatch/entry/confirmation, scheduling and handler bounds with stated headroom. Eventual fairness is insufficient for numerical deadlines. Test `limit−1`, `limit`, and `limit+1` at every resource boundary and prove that resource exhaustion still leaves enough control capacity to report or clean admitted work.

## Implementation sequence and completion gates

These are dependency-ordered work packages, not calendar estimates. Each ends with reviewable code, its stated tests and an updated evidence inventory; no package is complete merely because a model previously passed.

| Package | Work and dependency | Completion gate and principal tests |
|---|---|---|
| P0 Contracts and architecture selection | Resolve schema-critical Q03–Q09; define Q01/Q02 scope and candidate Q11–Q14/Q23 contracts. Prototype runtime entry/cancellation, lifetime accounting and footprint; record reuse/adapt/replace decisions. | Coherent new API/ABI/ownership design and measured feasibility for critical mechanisms. No legacy compatibility adapter or old-code repair prerequisite. |
| P1 Shared core and deterministic harness | Establish the new core skeleton and minimal host/embedded builds. Add injectable clock, action barriers, resource counters and trace schema; can begin alongside P0 prototypes. | Same core builds through both ports; harness separates dispatch/execution/evidence acceptance and observes resource ownership. BASE/OBS tests target the new design. |
| P2 Contracts and admission | Versioned immutable requests, bounded decoder/P evaluator, stable identities, O/K reservation algorithm, complete incidence/fences and atomic capacity/result reservations. Depends on P0/P1. | Invalid input cannot allocate managed objects or dispatch; duplicate requests cannot restart; concurrent preflight traces cannot over-admit. REQ/PRED/CON admission and RES tests. Refine Admission model to chosen algorithm. |
| P3 Runtime service and no-edge operations | Implement the selected runtime/guest ABI, inert preparation, entry hook, cancellation/closure, phase guards, history and deadlines. Node add/remove with no edges. Depends on P2 and Q11–Q16/Q23. | HP01/HP02; F02/F09–F12; C03–C05; actual runtime entry and safe closure demonstrated under allocation failure and cancellation. |
| P4 Endpoint service and edges | Implement endpoint pair preparation, permissions, receiver-first activation and independent scoped cleanup; edge add/remove between established nodes; data incarnation validation. Depends on P2/P3. | HP03–HP06; PERM/ID and C01–C06 tests; no peer runtime destruction; local and remote path parity. |
| P5 Multi-channel node addition | Add immutable P gates, independent work, PossibleSet, startup races and continuing optional-channel ownership after success. Depends on P3/P4. | HP07–HP12, PRED, F00–F15/C01–C06, optional deadline tests and all three failure histories. |
| P6 Full node removal and overlapping work | Compute/fix complete current scope, fence additions, cancel pending creators, preserve historic successes, aggregate every closure obligation. Depends on P4/P5. | HP13–HP16, CON01–CON15, REM tests including pending optional work and distributed authorization in flight. |
| P7 Reporting and retirement | Query/retain attributed outcomes, O outage handling, bounded result policy, source revisions, authorized reconciliation, safe detailed record retirement and reuse. Schema begins in P2; finish on P5/P6. | MGMT/ID/RES tests, original deadlines unchanged, old control/data ineffective after reuse; refine BindingReuse model. |
| P8 Simulator integration and applications | Replace or adapt simulator O client, native integration and observer against the new contracts as P2–P7 land. Recompile workloads for the new ABI and translate scenario intent; add control/data impairment, constrained profiles and ventilation evaluation. | New API exercised concurrently; protocol evidence matches actual core events; application tests pass where criteria are defined. Obsolete client/snapshot/fixture formats are not release gates. |
| P9 Target qualification | Run the same protocol suite through host and embedded adapters, sanitizers, allocation/churn tests, target memory/stack/scheduling measurements, and selected hardware-in-loop faults. Depends on functional packages. | Capacity ledger fits selected device; bounded handlers/allowances supported by measurements; no missing mandatory closure/identity test; explicit remaining verification limits. |

Do not postpone cleanup until after the happy path: each allocation/start feature ships with its cancellation, partial-failure and retained-evidence behavior. Similarly, enable new concurrent simulator operations only with stable protocol correlation, not with the existing globally serialized host command sequence as a substitute.

### Adoption of the replacement

Use an isolated build target while bringing up the new core. Switch the simulator's device executable and workloads to that target when a vertical slice includes its success, failure and cleanup behavior. Keep earlier artifacts as a comparison baseline where useful; the two implementations need not interoperate or share writable runtime records. When the new implementation meets the conformance gates, remove superseded build paths, wrappers and tests tied only to discarded internals, preserving tests of required observable behavior. Runtime cutover, record conversion and compatibility support are additional requirements only if later requested.

## Test and verification strategy

The [test catalogue](graph_operation_test_plan.md) is the implementation acceptance specification. Use small deterministic core tests for decision boundaries and ownership; parser/property tests for the selected encoding; real KaOS/selected-WASM-engine process tests for distributed operation; workload tests for service; and target measurements for resource/timing claims. Retain the existing TLC suites and add implementation-specific admission/lifetime/reuse refinements where abstractions are replaced. Existing tests are reused or translated only when they check retained requirements; do not make the old implementation the correctness oracle for new behavior.

Map concrete events to model actions: accepted immutable request → AcceptAddition/AcceptRemoval; owner preparation → Prepare/ObserveReady; recorded commit → Commit; activation authorization/owner transition/accepted report → IssueActivation/Activate/ObserveActive; start → Dispatch/Execute/EmitStarted/SucceedAddition; removal → SendCleanup/RecordRemoval/Close/ObserveClosed/FinishCleanup. Names differ between suites; preserve their boundaries rather than requiring a specific C function name.

Every run records source and configuration hashes, profile, clock mode, deterministic schedule or random seed, faults actually injected, expected outcome, observed outcome/evidence, outstanding obligations, memory ledger, event loss, and test duration. A missing event is uncertainty; it cannot prove absence of execution or safe closure. Require explicit reachability of each success/failure history, so safety tests cannot pass merely because nothing starts.

Expected negative model failures have two translations. A guard-removal trace becomes a C regression where the conforming implementation prevents the bad state. An assumption-removal trace becomes a test that preserves safety while declining the unsupported progress claim, usually reporting Unresolved when Kc/timers remain live. Do not deliberately require a live conforming implementation to deadlock in order to “match” a weakened model.

## Coverage gaps and additional cases

“Not formally covered” differs from “missing in prose,” and neither automatically means the protocol is defective.

| Gap | Evidence boundary and required addition |
|---|---|
| Distributed admission and more than two operations | VG coordinators reside on K0 and admission is abstract. Add multi-coordinator reservations, delayed authorization, three-way conflict/fence races and partial initial fan-out. CON/REQ tests and a concrete admission refinement. |
| Rich Boolean dependencies combined with later graph operations | VN has up to three channels and richer P; VG's combined model has a required or optional single-channel addition. Add optional expiry after parent success concurrent with removal, rich P with later removals, and shared-host capacity. PRED/CON/MGMT tests. |
| Truthful startup and shared-memory closure | Abstract STARTED/CLEANED do not verify the selected runtime entry hook, callback ownership, buffer lifetime, guest initialization effects or allocator. F10–F12, LIFE and RES tests apply to every replacement. |
| Parser and identity implementation | Models assume valid immutable authenticated requests and abstract old-traffic isolation. Add malformed/truncated/oversized messages in the selected encoding, duplicates with changed bodies, replay, wrong owner/port/instance, endian/width issues and counter exhaustion. REQ/ID tests. |
| Result query/retirement and bounded storage | Prose recognizes the obligation; executable models do not specify a production policy. Add O outage pressure, result serialization pressure, retired queries, retention with unresolved optional work, and delayed traffic after full-record reclamation. MGMT/RES tests. |
| Additional boundary cases identified in this review | Exact-deadline tie policy; source-revision and clock rollover; stale callback delivered after record reuse; outcome-event loss at queue saturation; authorized-but-not-yet-dispatched edges in removal incidence; optional fan-out starving a sufficient subset; body equality under alternative allowed encodings; report loss during shared cleanup; artifact staging exhaustion. These are explicit new cases in the catalogue, with decision-dependent oracles marked. |
| Application service and security boundaries | Ventilation thresholds, fallback, command outcomes, clock uncertainty and plant fidelity are not selected. Guest import/resource isolation is not enforced by simulator resource declarations. Add application and capability tests, without claiming Byzantine KaOS tolerance or actuator correctness from protocol success. |
| Environment fidelity | Test control-plane loss, duplication, deterministic phase races, multi-host clock behavior, resource caps, and shared-core host/embedded parity. Radio, fragmentation, bandwidth contention and hard real-time behavior need additional environments if those claims are required. |
| Restart, migration and recovery | Explicitly excluded by the verified scope. Record as future specification work under Q02/Q22, not as defects to patch into the six operations implicitly. |

Completion means the six operations meet their evidence and lifetime rules in the chosen bounded profile, every required test has a disposition, and application claims use approved workload criteria. The current finite checks and planning audit do not establish that completion today.
