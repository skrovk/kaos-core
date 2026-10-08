# Graph operation glossary

Shared vocabulary for the graph-operation documentation, C core and Python orchestrator. Definitions describe the agreed design; they do not imply that every service is implemented. The [implementation plan](graph_operation_implementation_plan.md) records scope and qualification gates, and the [P2 contract](graph_p2_contract.md) specifies the current wire format and bounds. The [design questions](graph_operation_design_questions.md) retain detailed decisions and accepted limitations.

## Actors and authority

| Term | Meaning and implementation mapping |
|---|---|
| O / orchestrator | The authority that allocates identities and authorizes graph changes. It records intended bindings and removal fences before dispatch. Implemented by [`Orchestrator`](../../sim_env/sim_env/orchestrator.py); its graph view is not direct knowledge of remote execution. |
| K / KaOS | The device-side system that admits work, hosts nodes/endpoints and enforces local ownership and permissions. The shared C implementation is in `graph_core/`. |
| Kc / coordinator | The KaOS coordinating one operation: it collects owner evidence and publishes that operation's aggregate result. `core_report.coordinator` identifies it. Coordination is a role for an operation, not a separate device type. |
| Ki / participating owner | A KaOS hosting an object in the operation's scope. It independently admits its local work and reports local facts. One device can be both Kc and Ki. |
| Object owner | The hosting KaOS responsible for the particular node or endpoint and its truthful evidence. `core_owner_report.owner` identifies it. “Owner stopped” concerns this object, not all activity on that device. |
| Cleanup owner | The local record/service responsible for releasing a resource, including partial allocations. Each allocation has one cleanup owner; other users must retain tracked access. This is resource ownership within K, distinct from identifying the hosting device. |
| DB / collector | The designated evidence/log storage service in the design. Its storage acknowledgement is separate from an operation outcome. Collector/eviction integration is later work. |
| Guest / runtime / host | The guest is the application module. The runtime executes it; WAMR (WebAssembly Micro Runtime) is the selected qualification candidate. “Host” can mean the surrounding native system or the POSIX simulation platform, depending on context. A host import is a function exposed by that system to the guest. |
| Control plane / data plane | Control manages graph operations, permissions, deadlines and evidence. Data activity carries application values through authorized channels. Data load must not prevent admitted control and cleanup work from progressing. |

## Graph objects and identities

Types are defined in [`kaos_admission.h`](../graph_core/include/kaos_admission.h) and Python's [`graph_protocol.py`](../../sim_env/sim_env/graph_protocol.py).

| Term | Meaning and implementation mapping |
|---|---|
| Logical node / N | An application graph node identified independently of a particular execution. `logical_node_id` / `Node.logical_id`. Distinct concrete instances may share a logical ID. |
| Instance / instance ID | One concrete creation of a logical node. `core_instance_id` / `InstanceId`. In this schema, its ID equals the creating operation's ID. Reusing a physical slot does not reuse the instance identity. |
| Operation / operation ID / opId | One immutable authorized graph change. `core_op_id` / `OperationId`. O allocates non-recycled unsigned 64-bit IDs; zero is valid. New intent, including removal, receives a fresh ID. |
| Six operations | Node addition with or without edges, edge addition, node removal with or without edges, and edge removal. `core_request_kind` / `Kind`. The no-edge removal specialization requires complete empty incidence. |
| Port | A declared input/output on a node, with a local ID, direction, type and payload bound. `core_port` / `Port`. Distinct channels may use the same port; this does not introduce implicit fan-out. |
| Channel / directed channel ID | A directed connection identified by `(sourceLogical, destinationLogical, serviceId)`. `core_channel` / `Channel` also carries concrete instances, owners, ports and capacity/allowance metadata. |
| Service ID | The discriminator within a directed channel ID; it permits distinct channels between the same logical nodes. It is neither an operation ID nor a port ID. |
| Binding | A particular creation of a channel, identified by `(directedChannelId, creatingOpId)`. `core_binding` / `Binding`. Cleanup must match this exact identity to avoid affecting a later or sibling binding. |
| Endpoint / endpoint role | The local sending or receiving side of a binding. `core_reserved_endpoint.sender` selects the role; Python `Endpoint` identifies its node instance, owner and port. Even a co-located channel has two roles. |
| Manifest | The node's declared ports and, when present, channel descriptors, bounds and dependency predicate. Admission validates the complete declaration before reserving resources. |
| Dynamic binding | Adding a channel to an established instance whose manifest allows it. `dynamic_ports` / `Node.allow_dynamic` records this permission; a fresh authorized operation and local admission are still required. |
| Incidence | All intended bindings touching an exact node instance, including authorized but unsent, preparing and unresolved bindings. O's complete intended incidence determines node-removal scope; a local registry scan alone cannot establish it. |
| Scope | The exact immutable objects covered by an operation, represented by `core_request_scope`. Evidence order is the optional node followed by source/destination endpoint roles for each listed binding. |
| Predicate / P | The monotone Boolean condition over declared complete channels, represented by `core_predicate`. The same condition is evaluated over prepared, active or still-possible channel sets at the appropriate decision. AND/OR, constants and channel references are supported. |
| Optional channel | A declared channel that need not belong to every set satisfying P. Its work and cleanup obligations persist even when other channels suffice for node success. |
| Fence | Retained state that prevents conflicting new authorization or obsolete construction. O fences incidence during removal; K retains exact instance/binding cancellation state. This is not a CPU memory fence. Protection after discarding history is limited by Q05. |

## Lifecycle and outcomes

Evidence constants and validation live in [`kaos_report.h`](../graph_core/include/kaos_report.h) and [`kaos_report.c`](../graph_core/kaos_report.c). Preparation, activation and startup execution services are later implementation slices.

| Term | Meaning and implementation mapping |
|---|---|
| Authorization | O's decision permitting a specific immutable operation and scope. It does not guarantee that any K has admitted the work or has capacity. |
| Admission / reservation | K validates the request and atomically reserves its complete local requirements, including evidence and cleanup capacity. `core_request_submit` and the admission pool implement this boundary. Acceptance alone permits neither guest execution nor endpoint activation. |
| Rejection / duplicate | Rejection refuses fresh admission. A retained top-level duplicate ID is discarded without restarting work. `CORE_ADMIT_*` statuses are admission results, not operation outcomes. Missing replies do not prove rejection. |
| Preparation / READY | Successful inert preparation of an object, recorded as `CORE_OBJECT_READY`. Node preparation includes the selected runtime requirements; downloading an artifact alone is insufficient. Prepared endpoints are not yet active. |
| Commit | Kc's recorded decision that the operation's preparation conditions hold. `CORE_HISTORY_COMMIT`. It is a protocol decision, not a database transaction, Git commit or proof that the guest has run. |
| Activation / ACTIVE | The owner enables an endpoint under matching authorization and current guards. `CORE_OBJECT_ACTIVE` records that fact; its evidence requires READY. |
| Dispatch | Sending or scheduling authorized work. O's `dispatch` hands the immutable request to transport; startup dispatch schedules guest entry and is recorded by `CORE_HISTORY_DISPATCH`. Neither boundary proves execution. |
| STARTED / INIT marker | Accepted evidence of actual authorized main entry. The agreed convention uses an empty guest `INIT` marker as the first action after required configuration succeeds, attributed and checked by K. `CORE_HISTORY_STARTED`. Missing markers do not prove nonexecution. |
| Cleanup selected / closing | A decision to stop construction/activity and discharge obligations. Whole-scope selection is `CORE_HISTORY_CLEANUP`; local owner selection is `CORE_OBJECT_CLEANUP`. Closing a lifetime blocks new access but does not end existing accesses or establish CLEANED. |
| Cancellation | Suppressing pending work or requesting termination of active work. Cancellation before preparation retains a fence. A cancellation/termination request must be followed by observed completion of affected work before declaring it stopped. |
| Stopped / owner stopped / STOPPED | New work and access for the indexed object are blocked, and all in-flight execution, callbacks and tracked accesses have ended. `CORE_OBJECT_STOPPED = 16`. Resources may remain allocated; other objects on the hosting device may continue running. |
| Reclamation / release | Freeing owned allocations and releasing the relevant charges, associations and reservations after accesses have stopped. Local safe release need not wait for unrelated remote evidence. |
| CLEANED / object closure | The object is stopped and its required owned resources, associations and reservations have been released. `CORE_OBJECT_CLEANED = 32`. Retained management evidence can outlive object resources. |
| Distributed closure | Accepted CLEANED evidence for every object in the fixed cleanup scope, including remote endpoint roles. Local release alone does not establish it. |
| EXITED / runtime exit | Observed return or trap from the runtime, represented by `CORE_OBJECT_EXITED`. Other accesses or resources can remain; exit alone establishes neither STOPPED nor CLEANED. Historical success is preserved. |
| FAILED / EXPIRED | Object facts recording failure or applicable allowance expiry: `CORE_OBJECT_FAILED` / `CORE_OBJECT_EXPIRED`. Neither establishes stopped activity or released resources. |
| Pending | `CORE_OUTCOME_PENDING`: no final success, full failure closure or unresolved cleanup cutoff has been established. |
| Succeeded | `CORE_OUTCOME_SUCCEEDED`: the operation's success conditions were established. Addition success is historical, not a claim of current health; removal succeeds on full scoped closure. |
| FailedClean | `CORE_OUTCOME_FAILED_CLEAN`: an unsuccessful addition's whole cleanup scope has complete closure evidence. A local failure alone is insufficient. |
| Unresolved | `CORE_OUTCOME_UNRESOLVED`: the cleanup cutoff was reached with missing closure evidence. Obligations remain; later closure can resolve the result. It is not permission to forget or reuse unsafe resources. |

The cleanup evidence prerequisites are `CLEANED ⇒ STOPPED ⇒ CLEANUP selected`. These are cumulative facts, not three mandatory messages. For example, a stopped guest with all accesses drained but a still-allocated runtime is STOPPED; releasing its remaining required resources permits CLEANED.

## Work, ownership and time

Lifetime and worker primitives are defined in [`kaos_graph.h`](../graph_core/include/kaos_graph.h); transfer ownership is defined in [`kaos_artifact.h`](../graph_core/include/kaos_artifact.h).

| Term | Meaning and implementation mapping |
|---|---|
| Lifetime / ownership ledger | `core_lifetime` tracks closure, allocation charges and outstanding access tickets. It is accounting and access control, not an allocator or a complete protocol outcome. Associations outside the ledger also gate CLEANED. |
| Charge / quota | A reservation counted against bounded resource capacity before allocation/work begins. `core_lifetime_charge` records it; actual release precedes uncharging. Static records are additional to the variable-byte quota. |
| Tracked access / ticket | `core_access` identifies one admitted use of a stable lifetime record. Cleanup blocks new tickets and drains old ones. Ticket sequences do not wrap or reset on slot reuse, so stale callbacks cannot release a new access. |
| Unclaimed | `core_lifetime_unclaimed`: no outstanding access tickets. An open lifetime can be unclaimed and still admit new access; this predicate alone does not establish owner STOPPED. |
| Reclaimed | `core_lifetime_reclaimed`: the lifetime is closed, its access tickets are drained and its allocation ledger is empty. External associations and protocol obligations still need their own checks before reporting CLEANED. |
| Action / completion mailbox | `core_action` holds one issued worker action through `ISSUED`, `EXECUTING` and `COMPLETED` until control accepts it. Completion remains stored without allocating a queue slot. Worker success is separate from graph-operation success. |
| Completion acceptance | Control consumes the matching worker result and releases its tracked access. A late result cannot publish into a closed object; private allocations still need disposal. |
| Artifact / artifact reference | The immutable guest bytes and O's opaque identifier for them. Length and CRC metadata bound and validate transfer. The reference is not a node identity or runtime pointer. |
| Staging | Storage reserved for artifact bytes during preparation. Its allocation remains charged while the loader/runtime needs those bytes. Partial and failed transfers still have an owner. |
| Artifact READY / handoff | `CORE_ARTIFACT_READY` means complete length/CRC validation, not protocol node READY. `core_artifact_take` releases transfer access and clears its pointer while handing storage responsibility to the node/loading owner. |
| Allowance / deadline | An allowance is a finite duration. A deadline is checked addition of that duration to the specified start time. Times are owner-local monotonic microseconds; one device cannot interpret another's deadline as its own clock. |
| Decision boundary / expiry | The serialized point at which a guard is evaluated. `now < deadline` is required; expiry wins at equality even if a message arrived earlier. Retries, queries and phase changes do not renew the deadline. |
| Cleanup cutoff | The deadline for the current cleanup/evidence attempt. Missing closure is recorded before incorporating late evidence. Cutoff stops that attempt's retransmissions, but safe local cleanup and acceptance of late evidence may continue. |
| Reconciliation | A fresh authorized management action to address uncertain or undesired objects. A query only observes; it does not authorize new construction or additional cleanup attempts. |

## Evidence and transport

See the [P2 wire contract](graph_p2_contract.md#wire-shapes), [`kaos_coap.h`](../graph_core/include/kaos_coap.h) and [`GraphTransport`](../../sim_env/sim_env/graph_transport.py).

| Term | Meaning and implementation mapping |
|---|---|
| Evidence / fact | An attributed historical observation whose truth must be established by its owning service. Report flags encode facts; decoding them does not establish sender authority, execution or closure. |
| Owner report | A cumulative snapshot for one indexed object from its hosting KaOS. `core_owner_report`, wire kind 10. Its revision belongs to that owner/object stream. |
| Aggregate report / result | Kc's complete cumulative snapshot of operation history, indexed object evidence and derived outcome. `core_report`, wire kind 8. It does not depend on receiving previous progress messages. |
| Revision / monotone | A revision advances only when new evidence is added. Facts accumulate without removing accepted history. Owner and aggregate revisions are separate streams; identical repeats consume no revision and same-revision conflicts are rejected. |
| Obligation | Work or evidence still required by the fixed scope. `core_report_obligations` identifies objects awaiting CLEANED during cleanup. Missing messages cannot discharge obligations. |
| Query / missing result | A read of retained evidence, wire kind 7. An absent retained result does not prove that an operation was never admitted, never executed or was cleaned. |
| Storage ACK | Application-level confirmation identifying the stored operation, coordinator and aggregate revision, wire kind 9. O returns it after incorporating evidence. Delivery/collector/eviction integration is later work. |
| Retirement / forgetting / eviction | Removing eligible retained management history. `core_admission_forget` requires caller-established eligibility and safe local release. Forgetting history is separate from stopping/reclaiming an object and can remove replay protection under Q05. |
| CBOR / schema | The bounded binary request/report encoding. Schema 1 uses fixed arrays, byte strings and shortest unsigned integers. The schema and selected profile constrain both Python and C. |
| CoAP / exchange | The datagram request/response protocol used for control and artifacts. An exchange tracks the original bytes, peer, identifiers, retries and deadline. Transport completion is separate from operation completion. |
| CON / transport ACK | CON requests acknowledgement of a datagram. A transport ACK confirms receipt and controls retransmission; an empty ACK contains no application response and proves neither admission nor evidence storage. |
| MID / token | A CoAP message ID matches datagram acknowledgements with their peer; a token correlates responses with requests. Neither is an operation ID or lifetime access ticket. |
| Block1 / Request-Tag | Block1 carries a request body in blocks. Request-Tag, peer and resource bind blocks to the same assembly. P2 uses this for `/graph` and `/prepare`. |
| Block2 / ETag | Block2 retrieves response data in blocks; ETag identifies the served representation across them. Artifact transfer checks sequence, stable ETag, exact size and final CRC. |
| CRC32 | The artifact corruption check, specifically CRC-32/ISO-HDLC over the staged bytes. It supplies neither participant authentication nor proof of execution. |
| Retry / restart | A CoAP retry retransmits the original exchange. An allowed artifact restart discards partial progress and uses a fresh attempt identity. Both remain bounded by the original applicable deadline. |

## Assumptions and verification

| Term | Meaning and where it is recorded |
|---|---|
| Crash-stop | The baseline in which a failed process does not recover its authority/state and resume. Reboot recovery and O takeover require additional design; process exit is not protocol CLEANED evidence. |
| Authentic participant assumption / Q08 | The baseline trusts enrolled participant attribution; the current UDP transport does not provide cryptographic authentication. Configured peer checks enforce the selected transport mapping. |
| Forgotten-history limitation / Q05 | After required rejection/cancellation history is discarded, stale construction may be admitted again. Non-recycled IDs alone do not prevent this. Still-retained fences and resource/lifetime safety remain required. |
| Profile / qualification | A profile selects finite supported bounds. Qualification supplies evidence that they work on the chosen target and workload, including peak overlap, stack, timing and radio costs. Host tests and successful cross-compilation do not establish physical ESP32 capacity. |
| P0–P9 / Q01–Q23 / R01–R08 | Respectively implementation milestones, recorded design questions and implementation/qualification gates. Their numbered sections are in the implementation plan and design-question document; a glossary definition does not mark a gate complete. |
| TLA+ / TLC / invariant | TLA+ specifies state transitions and temporal properties; TLC checks a finite configuration. An invariant must hold in every reachable modeled state. `Lifetime.tla`'s `StoppedActions` checks that no held tickets implies all action mailboxes are empty; it is not by itself the full protocol STOPPED guard. |
| Safety / liveness / fairness | Safety rules out bad transitions/states. Liveness requires eventual progress under stated assumptions. Fairness constrains scheduling of enabled actions; it does not make failed hosts recover or guarantee delivery on a permanently failed network. |
| Oracle / trace | An oracle supplies the expected result independently of the implementation under test. A trace records concrete or modeled events so a result or counterexample can be reproduced. Test-only knowledge must not become hidden protocol knowledge. |
| QoS / QoC | Quality of Service describes communication/service properties; Quality of Control describes effects on the controlled process. Meeting a delivery metric alone does not prove acceptable control behavior. Application criteria are specified separately in Q20–Q22. |

Historical verification JSON files preserve the terminology, logs and hashes of their recorded runs. They are evidence snapshots, not current API references.
