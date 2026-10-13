# P6 full node removal and overlapping work

P6 executes `REMOVE_NODE` (kind 5) through the existing admission, node,
endpoint and edge services. It adds no removal service, allocation pool,
background worker or protocol acknowledgement phase. The authority's existing
request format and complete-incidence transaction remain unchanged.

## Walkthrough: remove N with `A → N → B`

Assume N has instance 100, its incoming binding was created by operation 80,
and its outgoing binding by operation 100. O authorizes removal 200 with N's
exact instance and both original bindings. N's host coordinates five objects:

| Object | Meaning |
|---|---|
| 0 | N's runtime and node resources |
| 1, 2 | Incoming binding's sender at A and receiver at N |
| 3, 4 | Outgoing binding's sender at N and receiver at B |

1. `kaos_request.c` validates the fixed scope. `kaos_admission.c` checks it
   includes every locally known incident binding and reserves an operation
   record. Retained remote obligations count even when N's local endpoints
   were already reclaimed. The admitted scope fences new construction.
2. `kaos_edge.c` processes removals before pending creators. It selects cleanup
   for each exact binding and closes its local roles. `kaos_node.c` likewise
   processes node removals before queued startup markers, requests cancellation
   of N's existing worker, and waits for safe disposal.
3. `kaos_endpoint.c` disables permissions, drains queued payloads and waits for
   outstanding accesses before releasing endpoints. Its existing exact-binding
   fan-out records truthful local closure for all retained observers. A and B's
   runtimes and unrelated bindings remain usable.
4. Each remote owner receives one indexed binding under removal 200. It keeps
   that removal's own sparse record, installs the fence even if preparation
   never arrived, and reports its local role's cumulative facts. Object indices
   retain the coordinator's original ordering, including incoming senders.
5. Removal succeeds only when all five objects are CLEANED. If its original
   cutoff arrives first, its report is Unresolved. Later attributed closure can
   still advance that same report to Succeeded. Neither retry nor query changes
   the cutoff. There is no rollback or FailedClean result for removal.

The creating addition reports remain independent. A pending node creator
selects cleanup with its own Abort, CommitNoStart or StartIssued history;
pending edge creation likewise fails independently. An already successful
addition retains its historical Succeeded result after removal.

## Fixed scope and admission capacity

O already retains authorized bindings, including commands handed to transport
that have not reached either device. It freezes that incidence and fences the
target under its existing lock. Devices validate their locally known subset;
they cannot reconstruct unseen authorizations. The new process test delays an
authorized edge until after a removal reaches its different coordinator, then
checks that the retained fence rejects construction.

`current_incidence()` combines occupied endpoint reservations with retained
coordinator scopes whose remote roles lack accepted CLEANED evidence. It
deduplicates exact bindings in a stack array bounded by the configured channel
limit. The same calculation protects new admission and removal completeness.
Otherwise local reclamation could allow five unresolved bindings on a node
whose removal request can represent only four.

A remote endpoint owner retains only its local obligation; it does not wait
forever for another owner's evidence it was never responsible for collecting.
An accepted remote closure in another local coordinator record can discharge
admission's incidence bookkeeping. It is never copied into a different
operation's report. Existing report-retirement eligibility remains the caller's
responsibility; P7 must preserve closure knowledge when integrating eviction.

## Compact remote command

```text
POST /prepare
[4, 13, removalOpId, channelIndex, targetInstance,
 [channelDescriptor, creatingOpId]]
```

`channelDescriptor` retains its existing 13 fields. The target instance must
identify exactly one endpoint; that endpoint's owner must be the enrolled
sender, and the opposite owner must be local. Removal identity, target,
coordinator, channel index and descriptor cannot change on duplicates.

This command occupies at most 118 bytes under the current profile, inside the
existing 160-byte effect buffer. It carries no artifact, predicate or remote
clock. The new `core_endpoint_removal_submit()` API takes no time argument:
owners install cleanup fences, while the coordinator owns the wait deadline.

Each repeated command returns a fresh explicit owner snapshot, allowing a
response that initially contained CLEANUP to later contain CLEANED. Receipt
codes alone are not closure evidence. Compact commands use retained admission
records for idempotency and need no ordinary response-cache slot. Tag 13 is an
additive schema-4 command; peers that lack it cannot execute P6 together.

## Overlap and exit examples

If removal 200 expires at time 40 and removal 201 has cutoff 110, they share
physical endpoint/runtime reclamation but retain separate reports, issue masks
and cutoffs. Receiving a remote report addressed to 201 can make 201 succeed
while 200 remains Unresolved. Only evidence addressed to 200 resolves 200.
Late delivery is allowed; starting a new retry window for 200 is not.

If N succeeded and then exited, safe local reclamation can already have freed
N and its local endpoints. That does not prove A's or B's endpoints are gone.
A fresh authorized removal still includes those bindings, records local
absence as closure behind the retained fence, and contacts their remote owners.

Pending optional channels are part of the fixed removal scope even if no
PREPARE reached a peer. Removal before delayed PREPARE installs a fence without
allocating the endpoint. Established historical success is preserved; the
fresh removal supplies authority for distributed cleanup.

## Verification

New sources reuse existing test helpers without editing their cases:

- `tests/p6_test.c`: local and mixed placement, incoming/outgoing roles,
  unrelated traffic, three startup prefixes, pending optional work, pending
  edge creation at another coordinator, exit before removal, independent
  observers and late closure.
- `tests/p6_admission_test.c`: immutable sparse descriptors, removal before
  construction, observer exhaustion, retained remote incidence, bounded
  capacity, and the maximum-width command.
- `tests/p6_control_test.c`: incoming owner attribution, highest channel
  indices, held-access cleanup, fresh repeated snapshots with a full response
  cache, malformed requests and sender/path validation.
- `tests/p6_process_test.py`: four authority/UDP/WAMR cases for local and remote
  full removal, authorized work in flight and unavailable remote ownership.
- `p6_capacity.c`: four real WAMR guests, 16 bindings and 32 full queues; four
  overlapping eight-binding removals occupy all 24 operation records. Three
  cycles check full reclamation and preserved historical successes.

With explicit user approval, `tests/admission_test.c` now supplies remote
CLEANED evidence after local endpoint release in `complete_incidence_bound`,
before its existing successful-admission assertion. This discharges the old
binding from the per-node incidence count; it does not enlarge a removal
request. The other 41 pre-existing verification sources remain unchanged.
The new P6 admission test separately checks that local release alone leaves
the incidence limit full, and that remote closure permits a new binding.

Build the existing host suite in `verification/graph_core` and the separate
four-node suite in `verification/graph_core/four`. Build the new target fixture
with `bash verification/graph_core/espidf_p6/build.sh`. Exact results and hashes
are recorded in [p6_verification.json](../verification/graph_core/p6_verification.json).

The production structure layout is unchanged from P5. No physical P6 run,
Wi-Fi memory qualification or final composed TLC run is claimed. Those remain
target/formal qualification work; collector integration remains P7.

## Engineering review

Two design explorations independently recommended extending the current
services. A separate removal engine would duplicate ownership and evidence;
forwarding whole manifests would enlarge the retained peer buffers.

The implementation skill's separate standards/spec reviews found no hard
standards violations or new specification defects. Its interface simplification
finding was applied by removing the unused clock parameter. Generic suggestions
for polymorphism do not override the bounded embedded C design. No automatic
commit was made over the workspace's pre-existing implementation changes.
