# P2 contracts, admission and artifact transfer

Terminology: [shared glossary](graph_operation_glossary.md).

Implemented on 7 October 2026 against the [system plan](graph_operation_implementation_plan.md). This document specifies the bounded P2 implementation and its interfaces. P3–P6 remain responsible for runtime and endpoint execution, commit/activation/startup decisions, and complete distributed operation state machines. Artifact validation alone never establishes node READY or STARTED.

The selected approach extends the existing admission pool. A second generic registry was considered and rejected because it would duplicate identity, capacity and rollback state. O is a separate Python implementation in `../sim_env/sim_env/orchestrator.py`; it does not run K's protocol transitions. CoAP and CBOR are small fixed-profile implementations shared by host and ESP32. The vendored unicoap implementation depends on RIOT networking, event and timer infrastructure; adapting that stack would add more machinery than these bounded interfaces. No external C codec allocation or reassembly is hidden from the resource ledger.

## Ownership and interfaces

| File | Responsibility and main interface |
|---|---|
| `../sim_env/sim_env/graph_protocol.py` | Frozen node, port, endpoint and channel descriptors; distinct `OperationId`/`InstanceId`; canonical six-variant request encoding and static feature checks. |
| `../sim_env/sim_env/orchestrator.py` | `Orchestrator.add_node/add_edge/remove_node/remove_edge`, `dispatch`, `withdraw`, `rejected`, `query`, `apply_result`. One short lock protects authorization, immutable request association, complete intended incidence and removal fences. |
| `../sim_env/sim_env/graph_transport.py` | `GraphTransport.submit/query/add_artifact`: configured UDP peers, one outstanding client exchange per peer, independent artifact replies from the same socket. The receiver thread never decides a graph outcome. |
| `graph_core/kaos_request.c` | `core_request_submit`: complete bounded parsing before reservation; envelope validation before retained top-level duplicate discard. `core_endpoint_submit/cancel` handle independently issued owner work under the original authorization. |
| `graph_core/kaos_admission.c` | Fixed nodes, endpoint roles and operation/report records. Check every requirement, then publish once under the caller's lock. `close_scope` blocks new accesses; release requires drained lifetime and completion records. |
| `graph_core/kaos_report.c` | Cumulative attributed evidence, monotone facts, finite revisions, exact duplicate/conflict handling and derived outcomes. Callers must first establish the actual operation guards and owner provenance. |
| `graph_core/kaos_coap.c` | Datagram codec, bounded confirmable exchanges, response cache and bounded Block1 assembly. No sockets, allocation, clock reads or callbacks. |
| `graph_core/kaos_control.c` | CoAP resource routing into the same production request/query functions; bounded pre-admission decoder workspace. |
| `graph_core/kaos_artifact.c` | Sequential Block2 acceptance, exact length and staged-byte CRC validation, restart isolation, cancellation and explicit storage handoff. No allocator or runtime calls. |
| `graph_core/ports/p2_host.c` | Real UDP exercise port. Allocates charged staging after acceptance, drives transfer deadlines, and demonstrates local failure cleanup. It has no WAMR or endpoint execution. |

The core caller serializes short decisions. No core function holds a lock across network waits, guest execution or blocking allocation. The host exercise is a single event loop; the final control/runtime/worker scheduling and WAMR allocator integration belong to P3 and Q14 qualification. `core_node_admit` remains the original resource-only primitive for existing foundation callers. Production control enters through `core_request_submit` and a configured authority.

O records all intended bindings before exposing a request for dispatch, including bindings not yet sent. A node removal takes its complete incidence and installs its fence in the same locked decision. Definitely unsent withdrawal and matching original pre-work rejection release their bookkeeping. Missing responses/results retain uncertainty. `dispatch` hands off the recorded bytes once; the transport may retransmit that same CoAP exchange, but a changed intention needs a new operation ID. A closure learned before original dispatch cancels that definitely unsent construction. ID exhaustion refuses every fresh operation, including removals, without wrapping.

Local admission checks actual enrolled owners, exact instances, declared port direction/type/payload, dynamic-binding support, retained fences, complete known removal incidence, deadlines and every resource bound. Distinct instances of the same logical node may share a device. Distinct channel identities may use the same declared port; each data operation will address its particular channel, with no implicit fan-out. Pending and closing bindings count toward incidence until safely released. One local channel reserves two endpoint roles. Full original requests are forwarded to endpoint owners, which reserve all their local roles atomically without reserving another node or interpreting Kc's clock as their own.

## Static profile and admission bounds

| Quantity | Bound |
|---|---:|
| Resident local instances | 2 |
| Local endpoint roles | 8 |
| Retained operation/report records, including owner-only records | 4 |
| Declared channels per request and complete current incidence per instance | 4 |
| Ports per node | 4 |
| Enrolled owner IDs per K | 8 |
| Complete encoded request | 1024 bytes |
| Artifact | 1–65536 bytes |
| O artifact reference / staged guest configuration | 1–32 / 0–64 bytes |
| Channel payload / queue messages | 1–256 bytes / 1–4 |
| Predicate | 63 postfix tokens, depth 16; request channel indices 0–3 |
| Complete encoded result | 128 bytes reserved per record |
| CoAP datagram / generic Block1 workspace | 1152 / 2048 bytes |
| Artifact block | initially 256 bytes; server may downsize to 16–128 |
| Cached non-idempotent control responses | 4, retained for the exchange lifetime |
| Concurrent artifact pulls per peer | 1 |

The reusable predicate primitive still supports 16 channels; the admitted request profile is deliberately smaller. The per-instance variable-byte quota and total device budget are supplied once at pool initialization. Each endpoint role conservatively reserves 1024 variable bytes; shared-buffer optimization is deferred until endpoint implementation. The host exercise supplies 131072 bytes per node and 270336 total variable bytes. These are finite exercise settings, **not a measured supported ESP32 workload profile**. Staging is charged before allocation and remains charged after handoff while a future runtime needs its backing bytes. Static pool/report/decoder/exchange memory is additional.

The new record layout measured with the current ESP32 compiler is 5448 bytes for admission, 6848 for the control adapter, 40 for a report, 80 for an artifact transfer and 1208 for a client exchange. Corresponding host admission/control sizes are 5624/6888. Compiler-derived layout and function frames do not establish stack high-water, available Wi-Fi heap, fragmentation or workload feasibility. Physical T-Display measurements and WAMR staging/runtime overlap remain R05/P3/P9 work.

## Wire shapes

Schema version 1 is the single supported research schema. This finalizes the earlier unintegrated no-edge draft by adding explicit ports and dynamic-binding support; no legacy shape is accepted. Future accepted syntax changes require a version bump. Configured limit tuning alone does not.

CBOR uses shortest unsigned integers, definite arrays and byte strings only. Wrong types, unexpected fields, trailing bytes and non-shortest representations are rejected. No maps, tags, indefinite items, negation, threshold shorthand, executable predicates or codec recursion are supported.

Every operation is `[1, kind, opId, encodedBody:bstr]`. Op IDs and instance IDs are unsigned 64-bit values, including zero. A node's instance equals its creating opId. Binding identity is `(directedChannelId, creatingOpId)`, where `directedChannelId=(sourceLogical,destinationLogical,serviceId)`.

Common shapes:

```text
port = [portId:u16, direction:0(input)|1(output), typeId:u16, maxPayload:u16]
channel = [sourceLogical, destinationLogical, serviceId,
           sourceInstance, destinationInstance, sourceOwner, destinationOwner,
           sourcePort:u16, destinationPort:u16, typeId:u16,
           payloadBytes:u16, queueMessages, additionAllowanceUs]
binding = [channel, creatingOpId]
nodeFields = logicalId, artifactRef:bstr, artifactBytes:u32, crc32:u32,
             requiredPeakBytes:u32, configuration:bstr,
             additionAllowanceUs, cleanupAllowanceUs,
             [port...], allowDynamic:0|1
```

| Kind | Operation | Exact body |
|---:|---|---|
| 1 | No-edge node addition | `[nodeFields]` (10 fields) |
| 2 | Node addition with channel manifest | `[nodeFields, [channel...], predicate:bstr]` (12 fields) |
| 3 | Edge addition | `[channel, cleanupAllowanceUs]` |
| 4 | No-edge node removal | `[logicalId, instanceId, cleanupAllowanceUs]` |
| 5 | Node removal with complete incidence | `[logicalId, instanceId, cleanupAllowanceUs, [binding...]]` |
| 6 | Edge removal | `[binding, cleanupAllowanceUs]` |

Each predicate token is two bytes `(opcode, channelIndex)`: FALSE=0, TRUE=1, CHANNEL=2, AND=3, OR=4. Non-channel indices are zero. Operators are binary postfix. For example `(A or B) and C` encodes `02 00 02 01 04 00 02 02 03 00`. Validate complete structure, declared references, actual tree depth and `P(all declared channels)` before reserving anything. No-edge addition uses `P=true` internally. Logical self-loops are unsupported.

Times are owner-local monotonic microseconds. Positive allowances and all relevant additions are checked for overflow. Channel allowances cannot be shorter than their parent node allowance. Acceptance creates the original Kc deadline once; retry, query and artifact restart never extend it. Ki owner-only records have no Kc-derived local expiration. Cleanup gets its allowance at cleanup selection, or at acceptance for a requested removal. Expiry wins at equality at the actual decision boundary. Missing closure at cutoff is recorded before accepting a late closure, even if the later evidence resolves the outcome.

## Evidence and management contract

Full aggregate snapshots encode:

```text
[1, 8, opId, coordinatorId, aggregateRevision, outcome, historyFlags,
 [[objectRevision, objectFacts], ...]]
```

The immutable authorized request fixes exact object identities and owners: node first for node operations, then source/destination endpoint for each channel in request order. Therefore a result is independent of earlier progress messages while still being validated against the request. These are historical facts, not a simultaneous global observation or permission grant. Only Kc publishes this aggregate stream. Owner-only records do not answer aggregate queries. The distinct owner codec uses `[1,10,reportingOpId,hostingKaOS,objectIndex,objectRevision,objectFacts]`; P3/P4 publishers must establish actual transitions, and receivers must match the transport sender, indexed owner and issued action before observing it. Its revision is never compared with Kc's aggregate revision.

| History bit | Meaning | Object bit | Meaning |
|---:|---|---:|---|
| 1 | Commit recorded | 1 | Prepared/READY observed |
| 2 | Start dispatched | 2 | ACTIVE observed |
| 4 | STARTED accepted | 4 | Failure observed |
| 8 | Whole-scope cleanup selected | 8 | Owner cleanup selected |
| 16 | Cleanup cutoff with missing closure | 16 | Owner stopped |
| 32 | Historical addition success | 32 | Owner CLEANED |
| | | 64 | Runtime exit observed |
| | | 128 | Applicable allowance expired |

**Owner stopped** (`CORE_OBJECT_STOPPED`, bit 16) means new work and accesses for the indexed object are blocked, and all in-flight execution, callbacks and tracked accesses have ended. The hosting device can continue running other objects. Owned resources may still await reclamation. **Owner CLEANED** additionally requires releasing the object's owned resources, associations and reservations. A stop request, runtime exit or timeout alone proves neither state. The evidence prerequisites are `CLEANED ⇒ STOPPED ⇒ CLEANUP selected`; the execution service must establish each fact before reporting it. See the [lifecycle glossary](graph_operation_glossary.md#lifecycle-and-outcomes).

Object facts are emitted only when at least one new fact is added, so each object has at most eight advances after revision zero. Aggregate publication occurs only for a new object snapshot or one of six history facts. With at most nine objects, `8*9+6=78` advances fit an unsigned 8-bit revision. Initial removal CLEANUP belongs to revision zero. Revisions cannot exceed their cumulative fact counts. Repeats, retries, queries, storage ACKs and ordinary diagnostics do not consume revisions. This bound is a contract on future P3–P6 publishers: additional report-changing facts require updating the schema and its admission proof before enabling them.

Outcomes are 0 Pending, 1 Succeeded, 2 FailedClean, 3 Unresolved. Historical addition success remains terminal. Whole cleanup plus every object CLEANED means FailedClean for a failed addition and Succeeded for a removal; missing closure at cutoff is Unresolved. Late closure can resolve Unresolved. New commit/start/success cannot follow cleanup selection. Same-revision conflicts are rejected without replacing evidence or causing cleanup; the caller gets a bounded status code. Object provenance and actual STARTED/CLEANED truth still require the owning execution service.

Queries use `[1,7,opId,h'']`. Storage ACK identity is `[1,9,opId,coordinatorId,aggregateRevision]`; O returns this exact ACK only after validating/incorporating a complete snapshot. P7 will integrate ACK delivery, collector storage and pressure eviction. P2 protects all occupied records rather than silently retiring them. Current `forget` requires established external Q06 eligibility, discharged remote obligations and local release. Absence of a retained result is explicit and does not mean never admitted or cleaned.

## CoAP and artifact ownership

Configured peer attribution is a transport input, not a claimed body field. This is unsecured CoAP/UDP under Q08's authentic-participant assumption. The fixed implementation follows [RFC 7252](https://www.rfc-editor.org/rfc/rfc7252.html), [RFC 7959](https://www.rfc-editor.org/rfc/rfc7959.html), [RFC 9175 Request-Tag](https://www.rfc-editor.org/rfc/rfc9175.html) and [RFC 8949 deterministic CBOR](https://www.rfc-editor.org/rfc/rfc8949.html).

| Resource | Request and response |
|---|---|
| `POST /graph` | O's top-level CBOR operation; 2.01 accepted, 2.04 retained-ID discard, 4.00 invalid/conflicting request, 4.01 wrong authority, 5.03 insufficient admission or exchange capacity. These are not operation outcomes. |
| `POST /prepare` | Coordinator-issued complete original request to an owner, reserving local endpoint roles or scoped removal obligations. |
| `POST /cancel` | Coordinator-issued original addition selects owner cleanup, including cancellation before preparation. Same-op cancellation is a phase action, not discarded top-level resubmission. |
| `POST /query` | O's explicit query; 2.05 complete aggregate snapshot or 4.04 no retained aggregate. |
| `GET /artifact/<lowercase hex opaque reference>` | O serves immutable raw bytes with Content-Format 42, ETag, Size2 and Block2. |

Control requests use Content-Format 60. `/graph` and `/prepare` support Block1 with Size1 and a nonempty Request-Tag. Only one unfinished body is staged at a time; its original reception allowance is not refreshed. Its peer/resource/tag prevent mixed assemblies. Complete validation precedes admission. `/cancel` and `/query` are unsegmented: every supported body fits one bounded datagram, permitting idempotent replies even when the ordinary response cache is full. Duplicate final Block1 replies remain cached after releasing decoder storage.

Confirmable exchanges retry the original bytes with an initial 2–3 second interval, exponential backoff and at most four retransmissions, bounded by the original operation/exchange deadline. Empty ACK and response acceptance are separate. Tokens identify responses; peer plus MID identifies ACKs. The exercise ports never recycle MIDs or tokens during a run; exhaustion refuses new exchanges. Reopening a process is not supported recovery. A lost admission reply requires explicit query/reconciliation, not a new construction exchange. O's transport deliberately never treats a status code or timeout as automatic permission to discard authoritative bookkeeping.

After admission, the staging owner charges and allocates at most the declared artifact length. The transfer acquires a lifetime access. Each response must match the active peer/exchange, sequential block position, negotiated block size and stable ETag. Length and CRC-32/ISO-HDLC are checked over the bytes actually staged (`123456789` gives `cbf43926`). A successful `core_artifact_take` drops the transfer's access and pointer, leaving the allocation charged to the node/loading owner. Restart discards partial progress within the original allowance with a new attempt identity; stale responses cannot write into reused storage. The host allows one whole-transfer restart in addition to CoAP retransmission.

Fatal artifact failure selects the original whole-addition scope. Local cleanup cancels transfers, drains tracked work, frees storage, releases charges and associations, then records local closure. Remote obligations remain until matching evidence; local absence is never proof of remote closure. Exact-binding release leaves sibling bindings and peer runtimes untouched. An already-accepted requested node removal also cancels its unfinished creator. Normal P3–P6 execution will use the same lifetime and cleanup interfaces.

## Examples and verification

Build and run the actual C exercise port:

```sh
cmake -S verification/graph_core -B /tmp/kaos-p2-build -G Ninja \
  -DCORE_SANITIZE=ON -DCORE_BUILD_P2_HOST=ON
cmake --build /tmp/kaos-p2-build
ctest --test-dir /tmp/kaos-p2-build --output-on-failure
/tmp/kaos-p2-build/core/kaos_p2_host 58831 58830 1 99 2:58832
```

The optional `2:58832` enrolls a coordinator peer for owner-path tests. Device 1 replies to the actual enrolled sender; only O's port 58830 supplies artifacts. The host's stdout reports `ARTIFACT <opId> <bytes>` after validated handoff. It does not report startup. Until a P3 loader exists, an addition stays Pending and its original deadline eventually selects cleanup.

For example, O allocates addition 41 and instance 41, stores its immutable request and sends it once. K validates the whole request and reserves node, report and completion storage. It pulls a 600-byte artifact in blocks of 256, 256 and 88 bytes, checks the CRC and hands ownership to the future loader. O can query a Pending result without mistaking that artifact for a running node. A fresh no-edge removal 42 closes instance 41, cancels any outstanding pull, releases the staging allocation and records closure. The old addition can become FailedClean, while removal 42 becomes Succeeded. A new addition 43 can reuse the physical slot but cannot inherit old lifetime tickets or transfer tokens.

For a distributed example, a `/cancel` for creation 50 can reach Ki before `/prepare`. Ki retains the exact cancellation fence without allocating endpoints. The delayed preparation cannot restart construction. If the relevant record is later explicitly retired and all rejection knowledge is lost, Q05 still permits obsolete construction; the regression suite characterizes that limitation instead of claiming permanent replay prevention.

New C tests cover requests, predicates at the request boundary, capacity/incidence races, exact cleanup, revisions, CoAP, artifacts and deterministic host cleanup. New Python tests exercise O concurrency and a real UDP peer; process tests use Python O against this C port. Run the latter from `sim_env` with:

```sh
KAOS_P2_HOST=/tmp/kaos-p2-build/core/kaos_p2_host \
  uv run python -m unittest discover -s tests -p 'test_graph_*.py' -v
```

The new finite `verification/graph_core/model/Admission.tla` checks atomic capacity, retained identity, uncertainty and cleanup after staged work under fair completion/release. It is a deliberately small P2 refinement, not the final Q14 scheduler/runtime model. Existing lifetime models and all pre-existing tests are unchanged. Exact commands, source hashes, measurements and qualification limits are recorded in `verification/graph_core/p2_verification.json`.

P2's code and host contract checks are implemented. The stricter target-completion gate still needs physical ESP32 measurements. The compiled firmware fixture executes only the existing foundations; compiling P2 objects does not establish target P2 execution. WAMR feasibility, real radio behavior, runtime peak overlap and final Q14 safety/liveness remain pending. P3–P9 are not marked complete by this increment.
