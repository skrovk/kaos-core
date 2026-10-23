# KaOS graph architecture

The Python orchestrator owns authorization and intended topology. Each device
owns its local resources and execution. A device coordinating an operation
collects evidence from participating owners before declaring its outcome.

```mermaid
flowchart LR
    O[Python orchestrator] -->|Authorize and query| C[Device control]
    O -->|Immutable request metadata| DB[Host RAM collector]
    DB -->|Query and storage ACK| C
    DB -->|Original reports| O
    C --> A[Admission and evidence records]
    A --> N[Node service]
    A --> E[Edge service]
    N --> W[WAMR workers]
    E --> Q[Endpoint queues]
    W <-->|Copied guest data| Q
    E <-->|Commands and owner evidence| K[Other devices]
    Q <-->|Best-effort data| K
```

## Components and execution

P8 protocol experiments use `GraphRun` in the sibling simulator. It launches
the production `kaos_host`, exposes the existing orchestrator as `run.o`, and
records exact requests and accepted reports. One lock per coordinator covers
one management exchange. Waiting for an operation releases that lock between
queries, so other operations and devices can progress independently.

`GraphRelay` provides management and application UDP proxy sockets for each
device; O uses only management. Routes are `addresses[owner]["management"]`
and `addresses[owner]["application"]`. Everyone enrolls the matching proxy
addresses of their peers. The destination proxy identifies where a datagram
should go; the actual sending address identifies its source and socket class:

```mermaid
flowchart LR
    A[Actual host A] -->|Unchanged datagram| PB[Proxy B]
    PB --> R[Bounded delay, loss and duplication]
    R --> PA[Proxy A]
    PA -->|Source address enrolled as A| B[Actual host B]
```

This also routes O's admission, artifact, query and storage-ACK exchanges.
One policy and one set of packet-indexed random streams apply to every datagram
on a directed link. Control commands, artifacts, reports, storage ACKs and
application data share its loss, delay and duplication settings. Packet-type
labels are diagnostic only. A→B and B→A are independent, allowing asymmetric
loss; cutting both directions requires setting both to full loss. A fixed-capacity
queue shared across both classes charges both duplicate copies. A full partition also discards packets
already waiting for delivery.

The runner's `query()` hands evidence to O's existing validator, writes the
exact report into `events.jsonl`, then sends its exact storage ACK. A report
can therefore disappear from device history while O retains the accepted
facts. A query with missing history returns `None`; it cannot authorize a
fresh construction or prove cleanup. `wait()` returns an observed non-Pending
outcome, including `Unresolved`, or raises an observation timeout. Calling it
again requires fresh device evidence; callers retain reports already returned.
Its polling timeout is not a strict wall-clock bound under contention: another
call's exchange with the same coordinator can finish before the next query starts.

Closing a run stops processes and accounts for queued relay packets. It does
not add CLEANED facts. The runner treats an unexpected process exit as an
experiment error; it has no scripted crash/restart facility. Packet and stdout
logs support host experiments, with the observation limits described in
[verification](verification.md#p8-protocol-experiments).

| Component | Responsibility | Source |
|---|---|---|
| Orchestrator, O | Allocate IDs; record immutable requests; reserve intended incidence; fence removal | [orchestrator.py](../../sim_env/sim_env/orchestrator.py) |
| Shared host | Own the main loop, scheduling, module wiring and artifact/runtime handoff | [host](../graph_core/kaos_host.c) |
| Control and transport | Route management requests and replies; own bounded peer exchanges | [control](../graph_core/kaos_control.c), [CoAP](../graph_core/kaos_coap.c) |
| Artifact pull | Frame requests, validate blocks, bound restart and transfer buffer access | [artifact](../graph_core/kaos_artifact.c) |
| Platform adapter | Map addresses to enrolled IDs; provide datagram I/O, waits, retained wakeups and locks | [BSD sockets](../graph_core/host_udp.c) |
| Admission | Atomically check and reserve nodes, endpoints, notification capacity and operation records | [admission](../graph_core/kaos_admission.c) |
| Node service | Runtime preparation, startup decisions, cancellation and reclamation | [node](../graph_core/kaos_node.c) |
| Edge and endpoint services | Complete-channel evidence, activation ordering, data permissions, queues and scoped cleanup | [edge](../graph_core/kaos_edge.c), [endpoint](../graph_core/kaos_endpoint.c) |
| Runtime | Inert instantiation, guarded guest calls, termination and disposal | [WAMR adapter](../graph_core/runtime/kaos_wamr.c) |
| Evidence and retention | Cumulative facts, outcomes, aggregate revisions and eligible record retirement | [reports](../graph_core/kaos_report.c), [admission](../graph_core/kaos_admission.c) |
| Collector, DB | Retain exact report versions until O acknowledges them | [graph_collector.py](../../sim_env/sim_env/graph_collector.py) |

`core_host` owns one stable aggregate of admission, control, node, edge,
endpoint and staging state. The POSIX and ESP-IDF entrypoints configure it and
call the same `core_host_run()`. The loop uses `core_host_step()`, also exercised
with an injected clock and I/O in deterministic tests.

The shared socket adapter is `graph_core/host_udp.c`; its interface lives in
`graph_core/include/host_udp.h`. Each target's startup and `clock.c` implementation
live together in `ports/posix/` or `ports/espidf/`.
The protocol-only library has no socket dependency. Management requests are
CoAP-decoded once by the host and passed to `core_control_receive_message()`;
standalone callers can still use the raw-byte `core_control_receive()` interface.
Both retain the same replay, validation, authorization and admission order.
Application packets use raw DataFrames.

Ownership changes at explicit points:

| Resource | Owner | Transfer or release |
|---|---|---|
| Artifact staging buffer and charge | Host | Successful `core_node_artifact()` transfers both to runtime ownership; failed submission leaves them with staging. |
| Runtime instance, arena and artifact | Worker | Worker destroys them before publishing disposal; node code subsequently releases accounting and reservations. |
| Endpoint queue and permissions | Endpoint module | Close permissions, drain accesses, then release storage and exact binding roles. |
| Operation evidence and rejection fences | Admission/report modules | Retained beyond object release; an exact storage ACK and eligible retirement are separate decisions. |

```mermaid
flowchart LR
    Open[Open platform resources] --> Init[Initialize host and workers]
    Init --> Run[Bounded host rounds]
    Run --> Stop[Stop requested]
    Stop --> Cancel[Cancel all workers]
    Cancel --> Join[Join workers]
    Join --> Staging[Release remaining staging]
    Staging --> Close[Close platform resources]
```

Partial initialization unwinds only resources already created. Shutdown releases
process resources; it does not establish distributed CLEANED evidence or make
an old host context reusable.

One control loop performs short serialized decisions. Persistent workers serve
the bounded node slots; guest execution and runtime destruction happen outside
control transitions. Fixed mailboxes retain INIT and completion evidence even
when ordinary events are full. The host waits on sockets, runtime wakeups and
deadlines, servicing control and cleanup between bounded data batches.

Additional native imports are authorized per node instance. Admission and the
runtime worker each own a bounded copy of the granted names; the worker checks
the guest's module, name and actual function type before instantiation.
Native calls run on that worker and borrow guest memory only until they return.
Removal requests cancellation, then waits for worker disposal before releasing
runtime ownership. See [native imports](native-imports.md) for the API and
cleanup sequence.

Locks protect admission, permission checks and lifecycle changes. They do not
span guest execution or remote waits. On ESP32, control runs at priority 3 and
WAMR workers at priority 2. Continuously runnable transport work can keep the
host from blocking; worker progress under saturation has not been established
on the target. A management opportunity every round is not a CPU-time guarantee.

## Scheduling and task responsibilities

The existing control task owns admission, accepted evidence, lifecycle progress
and reclamation decisions. WAMR workers own preparation, guest execution and
runtime destruction; they publish facts in their mailboxes and retain wakeups
for control. At entry they take the registry lock, read current permission and
time, and keep that lock through WAMR's execution marker. They do not run node
or edge passes or consume another worker's mailbox. Guest data calls still
perform bounded, locked copies through the endpoint interface.

`core_host_step()` makes the scheduling order explicit:

```mermaid
flowchart TD
    Wake[Socket readiness, worker wakeup or timer] --> Before[Advance existing lifecycles]
    Before --> AppRx[Receive up to 4 application datagrams]
    AppRx --> MgmtRx[Receive 1 management datagram and apply consequences]
    MgmtRx --> Stage[Reclaim closed staging and start eligible artifact work]
    Stage --> AppTx[Bounded endpoint round-robin output]
    AppTx --> MgmtTx[Send replies and one full peer pass]
    MgmtTx --> Artifact[At most one paced artifact request]
    Artifact --> Maintain[Diagnostics, retention and next deadline]
    Maintain --> More{Budget exhausted?}
    More -->|Yes| Before
    More -->|No| Wait[Wait on both sockets, wake descriptor and deadline]
    Wait --> Wake
```

Only the host's `advance_graph_lifecycles_locked()` helper calls the paired node/edge passes.
Each checkpoint first calls `core_node_apply_cleanup()` to select node expiry,
removal and dependency cleanup without consuming runtime completion or INIT.
This prevents activation under a channel deadline that outlasts an expired node
startup deadline. It then performs two bounded edge-then-node rounds. The first can
publish endpoint READY and let the node commit; the second can activate its
endpoints and let the node dispatch. Edge removals are applied before node
startup evidence. This explicit dependency propagation handles local channels
without relying on another packet, another peer exchange or worker-side
scheduling. It is not an unbounded loop seeking a stable state.

Some control handlers also advance edge/endpoint cleanup synchronously to encode
an up-to-date reply. These remain on the control task. Worker disposal must be
observed before node STOPPED/reclamation; a cleanup cutoff never frees resources
still owned by a worker. An addition deadline governs startup, not the lifetime
of a successfully started node; unfinished channels retain their own deadlines.

`wait()` observes readiness without consuming packets. `recv(class)` consumes
at most one datagram and returns packet, dropped, empty, stop or error. Unknown
senders, oversized datagrams and malformed packets spend receive budget too.
The host checks due timers between packets and before each application send.
An exhausted receive or transmit budget selects deadline zero, requesting the
next round without depending on another external wake. A full finite peer pass
services every management peer; the existing endpoint cursor supplies persistent
application fairness by message, regardless of message size.

Both sockets send from their matching source port. The host rejects application
frames on management and CoAP on application. Authority and collector routes
may omit application; remote device routes require both addresses. Socket
separation does not reserve Wi-Fi airtime or isolate lower network buffers.

The production entrypoints space artifact send attempts by at least 1 ms.
One timestamp persists across blocks, retries, failed sends, restarts and new
transfers. Exchange/operation deadlines remain unchanged; terminal timeout
decisions bypass pacing. The next wait deadline includes the pacing timestamp
only when it postpones a send, avoiding repeated wakes on a paced retry.
Essential management and artifact responses still share one receive queue.
`core_host_config.artifact_interval_us` allows explicit tuning; zero disables
pacing for a baseline or adapter fixture. The RX/TX packet limits are bounded
compile-time settings in [kaos_host.h](../graph_core/include/kaos_host.h).

`core_host_stats` holds saturating counters and local timing maxima. POSIX emits
one bounded `TRANSPORT` line after shutdown with receipt/drop/invalid counts,
submission successes/failures, endpoint rejects, artifact attempts and timing.
Endpoint rejects include full, disabled, stale or invalid deliveries; they are
not all queue overflow. `artifact_complete_max_us` measures the final artifact
receive handler including checksum work, not checksum alone. Management-gap
maxima include idle waits and are observations, not a scheduling guarantee.
A missing summary
means those observations are unavailable. No retries are added to application
data, and successful submission does not establish remote consumption.

The pre-input and post-input checkpoints share the registry lock with packet
handling. No send or allocation occurs between them. The post-input checkpoint
therefore applies an admitted removal before a blocked worker can enter. Staging
free/allocation and network sends release the registry lock; clocks are sampled
again at the relevant decisions. A staging release gets a further checkpoint so
cleanup does not wait for a wakeup that will never arrive.

The platform adapter only waits, attributes packets, sends and wakes. It does
not decide which operation advances. Existing edge interfaces retain their own
deadline guards when issuing effects or accepting owner evidence, and control
handlers can finish synchronous endpoint cleanup before encoding its reply.
Those calls remain on the control task; moving them into workers or removing
them merely to reduce the count of `step` calls would change their contracts.

The host helpers separate artifact eligibility and handoff, management output,
data output, diagnostics and deadline selection. In the node module, named
checks describe local-role liveness, startup authorization and INIT acceptance;
entry and INIT share the same authorization predicate.

## Workload observation

The pipeline and feedback fixtures use the existing guest ABI. An optional
`core_host_platform.channel_observation` callback observes successful `put`
and `get` calls in the shared host, under the registry lock. It borrows the
payload only during the call and cannot change the operation result. The
POSIX host enables it with `KAOS_TRACE_CHANNELS=1`; `GraphRun` selects it with
`trace_channels=True`. The ESP-IDF adapter leaves it unset.

Each POSIX trace record contains a per-host sequence, host monotonic timestamp,
concrete caller instance, direction, logical channel and payload. One bounded
nonblocking write emits the record; failed writes increment a drop counter.
After workers join, a final record supplies total attempts and drops. Missing
records or totals prevent a complete-observation claim. This accounts for
channel traces only; ordinary diagnostic stdout and UDP kernel loss retain
their separate limitations.

The evaluator correlates observed GET bytes with a preceding PUT from the
expected source instance and checks the synthetic payload. GET establishes
copy-out to guest memory. It does not establish later guest validation or
physical actuation. Local POSIX processes share the host monotonic clock;
these observer timestamps are never supplied to guests. The
[workload walkthrough](workload-walkthrough.md) follows both fixtures and the
fresh-transform experiment through these boundaries.

## Identity and authority

A channel connects one sending endpoint to one receiving endpoint. Both roles
exist even on the same device. A node's *incidence* is the set of intended
bindings touching that exact instance, including pending and unresolved work.

| Identity | Meaning |
|---|---|
| Logical node | Application identity, independent of a particular execution |
| Operation ID | Non-recycled unsigned 64-bit value allocated only by O; zero is valid |
| Instance ID | The creating node operation's ID, retained for that concrete lifetime |
| Channel ID | Directed triple `(sourceLogical, destinationLogical, serviceId)` |
| Binding | `(channelId, creatingOpId)`; identifies the exact channel creation |

A removal receives a fresh operation ID and targets existing instances/bindings.
Storage slots are separate identities. Lifetime tickets and record incarnations
prevent delayed callbacks from acting on reused slots.

O records intended bindings before dispatch, including work still in transit.
Node removal atomically freezes that complete incidence and excludes new
authorizations. Device admission checks its locally known subset; a local
registry scan cannot discover all outstanding authorizations.

## Operation lifecycle

For a node with dependency predicate P, READY means inert preparation is
complete, ACTIVE enables an endpoint's data permission, and accepted INIT
establishes STARTED for the authorized main invocation:

```mermaid
flowchart TD
    A[Admit and reserve] --> R[Prepare runtime and endpoints independently]
    R --> C[Commit when runtime READY and P of prepared channels holds]
    C --> V[Activate receivers]
    V --> S[Accept receiver ACTIVE then activate senders]
    S --> D[Dispatch when P of active channels holds]
    D --> G[Recheck entry guards then configure and enter main]
    G --> I[Accept INIT before original deadline]
    I --> H[Historical success]
```

P supports constants, channel references, AND and OR. A qualifying channel
requires both endpoint roles. For `(A or B) and C`, either complete pair
`{A,C}` or `{B,C}` can support startup. Before dispatch, an alternative may still
become ready; after dispatch, known loss of all active support selects cleanup.
Optional channels keep their deadlines and cleanup obligations after success.

| Operation | Coordinator | Success condition |
|---|---|---|
| Add node without channels | Node host | Accepted STARTED; internally P is true |
| Add node with channels | Node host | Accepted STARTED with P satisfied |
| Add edge | Source host | Both endpoint ACTIVE facts accepted before expiry |
| Remove isolated node | Node host | Node CLEANED; complete incidence must be empty |
| Remove node with channels | Node host | Node and both roles of every scoped binding CLEANED |
| Remove edge | Source host | Both roles CLEANED; peer runtimes remain intact |

Cleanup closes access, cancels work, drains tracked users, releases resources
and reservations, then records CLEANED. Runtime exit or a termination request
alone is insufficient. Failed node construction includes every declared
endpoint, even one whose preparation was never observed. Overlapping removals
share physical cleanup but retain independent evidence and deadlines.

## Evidence and time

| Outcome | Meaning |
|---|---|
| Pending | Success or complete failure closure has not yet been established |
| Succeeded | Addition met its startup/activation gate, or removal obtained full closure |
| FailedClean | Unsuccessful addition obtained full scoped closure |
| Unresolved | Cleanup cutoff passed with missing closure; late evidence may resolve it |

Success is historical, not ongoing health. A successful node may later exit,
and its optional work may remain unresolved. Full distributed removal after
success requires fresh O authorization; safe local reclamation can proceed.

Owner reports accumulate facts for one operation/object/owner: equal sets repeat,
subsets are stale, supersets advance and incomparable sets conflict. Only the
coordinator versions the aggregate snapshot. Storage ACKs identify its exact
`(opId, coordinator, revision)`; they neither establish cleanup nor gate success.
ACKed live reports can become unavailable as history while operational state
remains protected. New evidence makes the newer version available again.

Device retirement protects live resources, pending work and unresolved closure.
Eligible acknowledged records are reclaimed first; admission pressure can then
evict the oldest eligible successful or fully cleaned failure record. Settled
remote-owner records retain cancellation fences until capacity is needed, and
require successful final closure-report delivery before retirement. If all
records are protected, new admissions receive backpressure. The collector keeps
every accepted report version until O acknowledges that exact copy; a newer
version does not replace an older unacknowledged one.

Deadlines use owner-local monotonic microseconds. Acceptance requires
`now < deadline`; equality expires. Retries, queries and phase changes never
renew allowances. Cleanup cutoff stops that attempt's retries while local
cleanup and acceptance of late closure may continue.

## Data and resource bounds

Guests address logical channels through copied, nonblocking `put` and `get`.
Each role owns a FIFO, including separate sender and receiver queues for a
local channel. Successful `put` means local acceptance; receiver overflow drops
the arriving message. There is no delivery, network-order, deduplication or
freshness guarantee. Channel notifications coalesce to the latest unread state.

The [static C profiles](../graph_core/include/kaos_limits.h) select:

| Limit | Default | Four-node profile |
|---|---:|---:|
| Resident nodes | 2 | 4 |
| Local endpoint roles | 8 | 32 |
| Operation records | 4 | 24 |
| Channels per request / incidence per node | 4 | 8 |
| Ports per node | 4 | 8 |
| Request bytes | 1024 | 2048 |
| Report objects | 9 | 17 |

Both profiles allow up to four 256-byte messages per endpoint. Node budgets
cover the retained artifact plus runtime arena; fixed pools, native stacks,
global runtime storage and platform costs are additional. Admission reserves
peak lifecycle requirements, and unreclaimed resources stay charged.

The current Python encoder retains the default four-channel, 1024-byte request
bounds. Enabling the larger C profile does not enlarge that encoder.

## Boundaries of the baseline

- Participants are crash-stop: a failed process does not recover and resume
  authority in the same deployment. Reboot/rejoin and restoration of O's authority
  are outside the implemented recovery contract.
- Configured UDP peers supply attribution under a trusted-participant
  assumption; cryptographic authentication is absent. CRC32 checks artifacts
  for accidental corruption.
- Discarding rejection history can allow an obsolete ADD/PREPARE to construct
  again. Retained-state and memory-lifetime checks still apply.
- Guests follow the trusted INIT-at-main-entry convention. The marker does
  not independently prove its source-level calling position.

The [recorded verification inventory](../implementation_rails/graph_operation_verification_inventory.csv)
identifies the evidence and scope of each demonstrated profile.
