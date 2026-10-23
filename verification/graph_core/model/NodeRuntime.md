# P3 node runtime refinement

`NodeRuntime.tla` models the no-edge P3 lifecycle under Q12/Q14/Q15/Q23 and
R02. It adds a finite execution arrangement to the earlier admission and
lifetime models; it does not replace or modify them.

The checked configuration has two immutable node/creator identities, one
runtime worker and reserved mailbox per admitted node, one worker lifetime
ticket per dispatched runtime, two charged allocation classes (artifact and
arena), at most one additional access per node, and a separate ordinary event
queue with capacity one. Counts stand for ownership categories rather than
physical byte sizes. Node slots are never reused in this finite configuration;
identity isolation is restricted to the retained history being modeled.

| Model boundary | Concrete boundary |
| --- | --- |
| `Admit` | P2 admission reserves the node, operation evidence and fixed runtime/mailbox capacity; operation and instance identities remain immutable. |
| `Dispatch` | `core_node_artifact` transfers a validated charged artifact, reserves the runtime arena, acquires the worker lifetime ticket and calls the runtime `prepare` adapter. |
| `BeginPrepare`, `Prepared`, preparation `Exit` | Runtime worker instantiation outside the control lock; mailbox `prepared` or `prepare_failed` publication. Implicit guest entry during instantiation is excluded by the restricted artifact profile and separately tested runtime integration. |
| `AuthorizeStart` | `core_node_step` / `execution_step` accepts successful preparation, records READY/commit/dispatch for the true no-edge predicate, and sets `start_requested` under the mailbox lock. |
| `BeginRun`, `EnterMain` | Worker checks cancellation, performs authorized configuration and enters the explicitly armed main invocation. No guest call executes under the control lock. |
| `EmitInit` | Host-attributed empty INIT writes the reserved `init_pending` mailbox; it does not establish startup success. |
| `AcceptInit`, runtime `Exit` | Control acceptance and worker `main_active`/`exited` updates share the same runtime mailbox lock. Acceptance requires authorized, active, nonclosing main and processing time strictly before the original addition deadline. |
| `Remove`, `Close` | Exact-instance O-authorized removal (including its priority control handling), pre-success runtime failure or addition expiry selects cleanup; runtime `cancel_requested` and lifetime closure exclude new entry/access. Post-success exit also permits safe local reclamation while retaining historical addition success. |
| `AdditionTime`, `CleanupTime`, `Cutoff` | Injectable monotonic clock comparisons and `core_node_step` deadline service. Due clocks are separate from timer-event service; queued evidence never receives an earlier acceptance time. |
| `Access`, `Drain` | Short serialized lifetime access admission/release. One extra access abstracts a callback or other user of this exact instance. |
| `Stop` | Queued execution is suppressed, or running work confirms completion after cancellation/termination. A termination request alone is insufficient. |
| `RequestDispose`, `BeginDispose`, `Disposed` | `cleanup` / `execution_step` uses `worker_only` to request disposal only after other accesses drain. Worker publishes `disposed` only after runtime calls/imports/callbacks have ended and artifact, arena and runtime resources have actually been destroyed. |
| `AcceptDisposed`, `Uncharge`, `Release` | `execution_step` accepts the retained worker action, releases its ticket, uncharges the freed allocation classes, releases the node reservation and only then records CLEANED. |
| `OrdinaryArrival`, `OrdinaryService` | Ordinary event storage is bounded and separate from reserved INIT/completion storage. Control and deadline service continue independently of ordinary queue occupancy. |

Preparation/configuration/main failure are represented by the appropriate
`Exit` transition; they all require the same closing, stopping and disposal
sequence. An idle admitted node may close before runtime dispatch. That case
has no runtime allocations or worker ticket to dispose. Artifact download and
its partial-resource cleanup are covered by the earlier admission/transfer
implementation and are abstracted before `Dispatch` here.

The invariants cover finite capacity, retained lifetime ownership, worker
pinning, authorization before guest execution, reserved evidence storage,
access drainage before disposal, and full reclamation before CLEANED. Action
properties additionally check acceptance-time startup guards, suppression of
entry after closure, charges retained through actual disposal, and sticky
historical success. A cleanup deadline becoming due before closure acceptance
forces cutoff to be recorded first; later CLEANED resolves the remaining
obligation without rewriting whether it missed that cutoff. Post-success local
reclamation alone has no cleanup/removal deadline; a fresh authorized removal
is required to create a removal deadline/outcome.

The following races are explicit finite traces, and should remain represented
by deterministic runtime/engine tests in `tests/node_test.c`:

1. `EmitInit; Exit; Close`: the pending marker cannot establish success
   (`marker_exit_race`).
2. `EmitInit; AdditionTime; Close`: a marker queued before expiry cannot be
   accepted after the deadline (`deadline_and_marker_guards`).
3. `AuthorizeStart; Remove; Stop`: cancellation wins before worker entry
   (`removal_wins_preparation`).
4. `AcceptInit; Exit; Close; ...; Release`: addition success remains historical
   while the unusable instance is reclaimed (`marker_exit_race`).
5. `Access; Close; Stop; Drain; RequestDispose`: outstanding access delays
   destruction; the worker ticket still remains held until disposal acceptance
   (`access_blocks_actual_disposal`).
6. `Close; CleanupTime; Cutoff; ...; Release`: cutoff reports an outstanding
   obligation; late reclamation can complete it (`success_remove_and_cutoff`).

Conditional progress properties require a live host, advancing deadline
clocks, fair serialized control service, each worker's eventual preparation
completion/cancellation/termination/disposal, and eventual return of tracked
accesses. Fair control service represents the implementation's service of due
work between bounded ordinary-event batches. The model deliberately does not
assume INIT emission, successful preparation, a fresh removal request or
remote delivery. A noncooperative main with no INIT can therefore time out and
be stopped under the explicitly assumed termination capability. If a worker
or access never returns, the safety properties still apply, but cleanup
progress is not claimed; the implementation must retain charges and report
Unresolved at cutoff.

TLC checks properties of a hand-written finite abstraction; no machine-checked
refinement relation from the C implementation or earlier models is claimed.
It does not establish C memory safety, the WAMR interrupt
implementation, scheduling latency or ESP32 memory/stack/timing bounds. It does
not model channel endpoints, transport retransmission, aggregate ACK/eviction,
replacement-slot identity reuse, or the final P4-P9 distributed arrangement.
The shared local cleanup is modeled once per instance; separate creator and
later removal report records and their individual deadline values remain
concrete engine-test obligations. `success` is the creator's historical
startup result, not a removal result. Fixed native worker stacks and retained
management storage are profile overhead outside the two managed charges.
The restricted artifact and trusted first-main-entry INIT convention remain
runtime/workload assumptions. Retained-history limits from Q05/Q08 still
apply. Recheck this model and its concrete mapping when those boundaries
change.

Run from the repository root:

```sh
java -XX:+UseParallelGC -Xmx2g -cp /opt/TLA+Toolbox/tla2tools.jar tlc2.TLC \
  -workers 1 -seed 42 -metadir /tmp/kaos-p3-node-tlc \
  -config verification/graph_core/model/NodeRuntime.cfg \
  verification/graph_core/model/NodeRuntime.tla
```

The run result and model/configuration hashes belong in the P3 verification
record. A fresh metadata directory avoids mixing results from edited models.

A separate one-node witness search reached the queued-marker/exit race in ten
states: `Init -> Admit -> Dispatch -> BeginPrepare -> Prepared -> AuthorizeStart
-> BeginRun -> EnterMain -> EmitInit -> Exit`. Its final state has
`initPending = TRUE`, `phase = "exited"`, `success = FALSE`, `held = TRUE`, and
`charges = 2`. TLC generated 880 states (302 distinct) in one second. This was
an intentional reachability check using a temporary copy plus the negated
witness invariant `~\E n \in Nodes : node[n].initPending /\
node[n].phase = "exited" /\ ~node[n].success`; its expected invariant violation
is not a safety failure of the checked model. The production configuration
checks that this reachable pending marker cannot subsequently establish
success. This witness maps to `marker_exit_race`; its full temporary output is
`/tmp/kaos-p3-node-witness.log`.

Final TLC check on 9 October 2026 completed with no errors: 3,061,705 states
generated, 496,008 distinct states, depth 40, no states left on the queue; all
nine invariants and six action/liveness properties passed in 12 minutes
11 seconds. The run used TLC 2.19 (2024-08-08, revision `5a47802`), one worker,
seed 42, fingerprint polynomial 74 and a 2 GiB heap. Log:
`/tmp/kaos-p3-node-tlc-final.log`. Java's local RMI socket required approved
execution outside the sandbox. Model and configuration SHA-256:

- `NodeRuntime.tla`: `83972cadfefede5e97e63391b3ede511f2875b85247ca824cb76a6595271c105`
- `NodeRuntime.cfg`: `79c28e1bbd2c4cf3c2b7fc29d66ac9f0f16144321b9c75a6d6e5370df03bd27e`
