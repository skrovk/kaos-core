# P5 multi-channel node addition

P5 extends the existing node and edge services to execute `ADD_NODE` (kind 2).
The coordinator keeps one immutable manifest, predicate and operation report.
The new node's instance and all initial bindings retain the original addition
opId. It allocates no child operation IDs and introduces no acknowledgement
phase for success. Full node removal remains P6; collector integration remains
P7; final formal and distributed target qualification remains P9.

## Walkthrough: `(A or B) and C`

Suppose N receives measurements through alternatives A and B and publishes
commands through C. O submits N's artifact/configuration, ports, those three
channel descriptors and the postfix predicate `A B OR C AND`.

1. `kaos_request.c` validates the immutable request. `kaos_admission.c` reserves
   the node, its local endpoint roles, notification capacity and one report.
   Runtime preparation and endpoint preparation then progress independently.
   Endpoint installation does not invoke N or wait for its artifact.
2. `kaos_edge.c` accepts attributed READY facts for each endpoint. A channel
   enters PreparedSet only when **both** roles are ready and the channel has
   no failure, cleanup or expiration. A's sender plus B's receiver do not
   constitute any complete channel. Missing replies alone leave a channel in
   PossibleSet.
3. `kaos_node.c` records COMMIT when the runtime is prepared and
   `P(PreparedSet)` holds. For example, A and C can support commit while B is
   still preparing. COMMIT grants neither execution nor data permission.
4. The channel service activates each eligible receiver, accepts its ACTIVE
   evidence, and only then activates its sender. This ordering applies to
   incoming, outgoing, local and remote channels. A receiver can buffer data
   before N runs. The existing copied FIFO service and guest ABI are reused.
5. N dispatches once when `P(ActiveSet)` holds. The supporting set is not frozen
   at commit: if A fails before dispatch, B and C may still support startup.
   Before dispatch, the operation waits only while `P(PossibleSet)` remains
   true and the original node deadline has not expired.
6. WAMR rechecks permission before configuration and main entry. Its entry
   guard takes the registry lock, services current channel/node decisions and
   checks accepted evidence plus current local node/endpoint lifetimes. WAMR
   then checks cancellation, preparation, exit and a freshly sampled deadline
   under the mailbox lock before marking execution. Both locks are released
   before guest code runs; the lock order is registry then mailbox.
7. The runtime's INIT marker is accepted only during that dispatched main,
   with a usable node, `P(ActiveSet)` and `now < originalDeadline`. Accepted
   STARTED records historical Succeeded. O/peer receipt acknowledgements and
   first application output are not part of this gate.

After dispatch but before accepted STARTED, loss of A is tolerated only if
another already-active support still satisfies P. If B is merely possible,
known loss of P selects whole cleanup immediately. The implementation does not
pause a partly started guest to wait for an alternative.

With `P=true`, the same engine can start N while every declared channel remains
pending. This differs from the no-edge operation: the declared channels still
retain their owners, authorization, reservations and deadlines.

## Responsibilities and bounded state

| Module | Responsibility |
|---|---|
| `kaos_admission.c` / `kaos_request.c` | Validate identity, placement, ports and capacity; reserve local objects; retain exact per-channel cancellation fences. |
| `kaos_endpoint.c` | Prepare local roles, copy data, enforce permissions, notify guests and reclaim safely. |
| `kaos_edge.c` | Coordinate indexed channels, validate owner evidence, enforce receiver-first activation, retain original deadlines and emit bounded transport effects. |
| `kaos_node.c` | Own runtime handoff/reclamation and immutable predicate gates for commit, dispatch and STARTED. |
| `runtime/kaos_wamr.c` | Execute through fixed worker mailboxes, perform entry checks and dispose runtime resources. |
| `kaos_control.c` / `ports/p4_host.inc` | Decode attributed commands and carry owned copies through the existing CoAP/UDP port. |

PreparedSet, ActiveSet and PossibleSet are computed from the retained scope and
facts; they are not separately mutable state machines. The edge service now
uses bounded object masks and a cleanup deadline per declared channel. Channel
setup deadlines are derived from the admission origin, not from first service:

```text
admissionTime = nodeDeadline - nodeAllowance
channelDeadline[c] = admissionTime + channelAllowance[c]
```

Admission checks overflow and requires channel allowances at least as long as
the node allowance. Whole cleanup has one shared cutoff in the admission
record, so servicing node and channel cleanup at different times cannot extend
it. Existing channel cleanup retains its earlier cutoff when whole cleanup
expands the scope. No retry or query starts another window.

The unused 128-byte report byte array was removed from every admission record.
The actual cumulative report remains. This recovers 3 KiB in the 24-record
profile before the new bounded bookkeeping. The final Xtensa structure probe
measures admission + node service + edge service at 39,280 bytes, down from
40,624 bytes in the starting P4 snapshot (1,344 bytes saved). This is a static
structure comparison, not a new physical heap measurement. The previous 256-byte payload,
four-message queues and 32 KiB global runtime arena remain unchanged; the
128-byte payload / 1 KiB arena proposal still needs its own qualification.

## Independent remote channels

A peer receives only its exact indexed channel, rather than another copy of
N's artifact, configuration, predicate and complete manifest:

```text
POST /prepare or /cancel
[4, 12, creatingOpId, channelIndex, channelDescriptor]
```

`channelDescriptor` is the existing 13-field channel array. These commands fit
the existing 160-byte effect buffer. A remote owner infers N from the endpoint
whose instance equals `creatingOpId`, requires its owner to match the enrolled
sender, and requires the opposite endpoint to be local. Preparation checks the
actual established peer node, port, incidence, notification and byte capacity.
Cancellation needs no live peer node or endpoint allocation.

One remote record holds a fixed-size sparse channel table for that opId.
`channel_known` identifies valid entries; `channel_cleanup` fences individually
cancelled entries. Each learned descriptor is immutable. A failure to admit A
does not prevent B from being admitted in the same record. CANCEL before
PREPARE records A's descriptor and fence without constructing resources;
subsequent delayed PREPARE cannot reopen it.

Activation remains `[4,11,opId,objectIndex]`. Object 0 is N; source/destination
roles of channel c are `1 + 2*c` and `2 + 2*c`. Owner reports retain the existing
cumulative attributed format. Receipt response codes never establish READY,
ACTIVE or CLEANED. A cancellation reply may carry an explicit owner report,
including current CLEANED on a repeated command. Compact prepare/cancel
commands use retained per-channel idempotency rather than consume another
ordinary CoAP response-cache entry.

This is an additive tag within schema 4. Existing request/report shapes and
P3/P4 commands are unchanged. A peer without tag 12 rejects P5 commands; a
mixed-version deployment is not supported. The public authority request
already supported kind 2, so no authority encoder or existing test changes
were needed.

## Failure, cleanup and reports

A terminal channel failure selects cleanup of both endpoints, including any
unobserved remote allocation. Before dispatch it fails N only when no sufficient
support remains possible. Whole failure closes N and every declared channel,
including those never ready. Prefix history remains derivable from COMMIT and
DISPATCH: Abort, CommitNoStart or StartIssued.

Coordinator cleanup selection adds aggregate CLEANUP obligations. Accepted
owner snapshots remain separate from those coordinator facts. Consequently a
READY already in flight can be accepted as historical evidence after selection
without conflicting with the locally recorded obligation or reopening the
channel. CLEANED still requires actual stopped work and reclaimed resources.

Pending optional work continues after Succeeded. Its original setup deadline
still applies. Optional failure/expiry closes only that pair and leaves N's
historical success intact. Missing optional closure remains visible in the
aggregate object obligations; an uncleared object's EXPIRED bit records expiry
of its setup or selected cleanup wait. A per-channel cutoff mask stops timer
work and retries once serviced; late valid CLEANED can still discharge an
obligation. Optional expiry does not set whole-operation CLEANUP/CUTOFF or
replace Succeeded with Unresolved.

After established channel/node success, a runtime fault does not authorize a
new distributed removal. Safe local reclamation continues; removal of the
remote established binding requires fresh O authorization. Obsolete
constructive exchanges stop when the channel is complete, and late command
rejections cannot select distributed cleanup of an already-successful active
channel. The explicit Q05 limitation after rejection-history retirement remains.

## Verification and reproduction

New tests preserve all test files present at the start of this slice:

- `tests/p5_test.c`: HP07–HP12 predicates and placements, complete-channel
  evidence, changed support, independent preparation, optional work, startup
  races, failure prefixes, exact deadlines, selective cancellation before
  preparation, retained cleanup cutoffs and late closure. Runs under both
  static profiles, including the highest indexed role.
- `tests/channel_admission_test.c`: sparse descriptor identity, independent
  admission, both directions, malformed commands, no-allocation cancellation,
  port rejection, stable duplicate reservations and owner-local clock rules.
- `tests/wamr_entry_test.c`: real WAMR entry suppression, lock release/order,
  allowed execution, and a worker blocked across its deadline.
- `tests/p5_process_test.py`: five real authority/UDP/artifact/WAMR cases with
  payload transfer, several channels sharing a remote owner record, and
  successful live nodes retaining unresolved optional cleanup after peer loss.
- `p5_capacity.c`: reuses the unchanged P4 qualification helpers and guest;
  constructs the fourth real WAMR node with eight initial channels, fills all
  32 queues, validates traffic, removes edges and then removes now-isolated
  nodes over three cycles. This does not exercise P6 full node removal.

Build host suites with the existing P2/P3/P4 host options and sanitizers in
`verification/graph_core`; the P4 host executable now runs P5 too. Build the
separate four-node fixture from `verification/graph_core/four`. Cross-compile
the new firmware with `bash verification/graph_core/espidf_p5/build.sh`.

The [verification record](../verification/graph_core/p5_verification.json)
contains commands, hashes, results, test-preservation checks and limitations.
No serial ESP32 was connected during P5 development, so the new firmware has
not been flashed or physically run. Earlier P4 physical measurements are
historical; they are not measurements of this P5 image. Wi-Fi memory headroom,
distributed physical placement, proposed memory-profile cuts, representative
application budgets and final composed TLA+/TLC verification remain open.

## Engineering guidance

The installed `implement` guidance was used for incremental checks and separate
standards/spec review. Review compared this session's starting file snapshot,
including pre-existing P3/P4 work, rather than treating all working-tree changes
as new P5 work. Review findings about deadline sampling across lock contention
and delayed post-success rejection were corrected and covered by new tests.

The generic review suggestion to replace repeated dispatch switches with
polymorphism is not a reason to add runtime allocation or a hierarchy to this
bounded embedded C implementation. Fixed arrays, explicit masks and existing
services remain appropriate. The skill's automatic commit/issue-tracker
workflow was not applied: this workspace already contained substantial
uncommitted implementation, and the user's task supplies the repository plan
as its specification. No pre-existing changes were committed or discarded.
