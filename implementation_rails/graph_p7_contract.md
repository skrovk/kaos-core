# P7 reporting, collector and retirement

Implemented on 12 October 2026, continuing the [P6 contract](graph_p6_contract.md)
and [implementation plan](graph_operation_implementation_plan.md). P7 adds exact
storage acknowledgement, bounded device history retirement and a host RAM
collector. It preserves the P5/P6 execution and independently authorized removal
paths. The current C/Python schema is **4**, as it was before this slice; older
schema-3 descriptions in the historical P2 evidence section are not the current
wire profile.

## Architecture and interfaces

```text
O (authorizations, current evidence)       DB (bounded host RAM archive)
          | /graph, /query, /stored          | /query, /stored
          +-------------------+--------------+
                              v
                     core_control (Kc)
                              |
             admission records and cumulative evidence
                       /                  \
               node service            edge service <--> Ki
                    |                       |
                  WAMR                 owned endpoints
```

There is no device exporter worker or second device result pool. The existing
record still owns scope, evidence and operational obligations. Three small
admission APIs implement the handoff:

* `core_admission_report_retained(record)` says whether its current version is
  still available as a historical result.
* `core_admission_acknowledge(pool, opId, Kc, revision)` acknowledges only that
  exact current version. Unknown, obsolete and mismatched keys do nothing.
* `core_admission_maintain(pool)` observes version ages and releases eligible
  acknowledged records. The host calls it at the end of serialized service.

Callers must serialize these APIs with admissions, evidence and node/edge
progress. Maintenance performs bounded scans over the fixed record pool; it
does no allocation, network wait or guest execution. `core_admission_forget`
remains the lower-level caller-controlled teardown API with its existing
resource-release check. Automatic retirement uses the stronger eligibility
checks described below; callers must not use raw forget as retention policy.

## Storage ACK and queries

O and the configured DB may POST `/stored` with CBOR
`[4, 9, opId, Kc, revision]`. A syntactically valid obsolete ACK receives 2.04
without changing newer evidence. It neither establishes cleanup nor changes
an outcome, deadline or revision. These idempotent resources do not consume the
ordinary admission-response cache. The latter remains bounded to four entries;
rapid fresh submissions can still receive backpressure until cache expiry.

POST `/query` uses the existing `[4, 7, opId, h'']` request. A retained result is
the unchanged complete cumulative kind-8 report with 2.05. Missing history is
4.04 with:

```text
[4, 14, opId, localDevice, observations]
observations = []
            or [revision, history, [objectFacts...]]
```

The optional observations describe still-present operational state. They do not
contain or reconstruct a historical outcome. A fully retired or never-known ID
has no observations. The collector returns `None` for missing history and does
not promote these observations to an archived result or release O's uncertainty.

For example, DB stores a successful startup report at revision 4, then ACKs
`(opId, Kc, 4)`. The device hides that historical version but keeps the live
runtime. Optional work or later cleanup adds facts at revision 5: revision 5 is
available again, while a delayed revision-4 ACK has no effect. DB retains both
copies if it accepted both; accepting revision 5 does not supersede revision 4.

## Safe bounded retirement

Coordinator records become eligible only when local resources are released,
the outcome is Succeeded or FailedClean, report obligations are empty and exact
object closure is known. A successful but still-live node/edge is protected.
Unresolved closure and pending optional work remain protected even after an ACK.
Retirement never clears an outstanding operation merely to make space.

Ki records require CLEANED and successful final owner-report delivery for every
known local role. They keep their rejection fences until a successful admission
needs the occupied slot; owner delivery alone does not trigger eager retirement.
Sparse unknown indices are not admitted local work. A later
original PREPARE can establish one of those roles after retirement, under the
documented Q05 limitation. Complete transport loss can keep a finished owner's
record charged indefinitely: P7 adds no retry wave or renewed deadline.

Maintenance releases eligible ACKed reports. Under admission pressure,
`select_record` prefers eligible ACKed records, then the oldest eligible retained
terminal version. Ages are bounded relative ranks assigned when maintenance
first observes a version; stable slot order breaks ties. ACKs and retransmissions
do not refresh age. This is neither numerical opId ordering nor a wrapping
wall-clock counter. A candidate is selected without mutation and forgotten only
when admission publishes successfully; a rejected request cannot evict history.

Removal and creator records can refer to the same physical binding. Before
discarding an observer, retirement copies its exact closure knowledge to other
retained references in `closed_objects`. This allows safe later admission without
inventing facts in another operation's report or changing its revision.
Independent removal attempts keep their P6 scopes, allowances and outcomes.

Each record slot has a non-wrapping incarnation. Edge effects carry operation,
slot and incarnation; all three must match before a late callback may act.
Checking incarnation alone is insufficient because two different slots can have
the same incarnation. Exhausted slots refuse reuse.

Kc also retains one compact final-owner response receipt per enrolled peer,
independent of operation history. If Kc accepted CLEANED, DB archived the result,
and Kc retired the operation before Ki received the reply, Ki's exact retry still
gets success. Matching includes MID, token, operation, object and facts; a retry
does not reapply evidence or extend the fixed exchange-lifetime expiry.

This bounded receipt design requires **NSTART=1 and strictly increasing,
non-wrapping MIDs per enrolled sender session**. Both supplied ports satisfy this
stronger adapter contract; generic CoAP does not imply it. Older delayed requests
cannot replace a receipt for a newer MID. Process/socket restart is not supported
recovery. A future port with random/recycled MIDs needs a different receipt policy.

## Collector realization (R06)

`sim_env.graph_collector.GraphCollector` is a host-side service driven by explicit
calls. It owns no thread, guest or persistence layer. The caller supplies a
separate `GraphTransport` socket and registers immutable `AuthorizedRequest`
metadata obtained from O. A device enrolls that socket with
`KAOS_COLLECTOR=<device-id>:<UDP-port>`; collector identity 0 is valid. This identity
may query and acknowledge storage but cannot submit graph work or prepare roles.
It uses no endpoint-owner peer slot.

The default bounds are 32 registered scopes, 32 report versions and 32 diagnostic
entries. Bounds are constructor parameters and remain finite. Reports are at
most 128 encoded bytes; each registered request is bounded by the Python request
profile (1024 bytes); each diagnostic is at most 256 bytes. The corresponding
encoded payload bounds are 4096, 32768 and 8192 bytes. Python objects, decoded
evidence, locks and transport storage add bounded overhead; these payload totals
are not a measured host RSS figure.

`collect(opId)` queries Kc, validates original attribution and registered scope,
reserves the exact report copy, then sends `/stored`. No staging queue grows
behind a full archive: `ArchiveFull` refuses the copy and sends no storage ACK.
A lost ACK reply leaves the archive copy retained. A failed collector/network
cannot gate graph completion or safe local cleanup. Devices may still discard
eligible old terminal history under pressure, so archival availability is not
guaranteed during an outage.

`retrieve(opId)` returns immutable original reports in revision order.
`acknowledge(ack, peer=O)` removes only the exact archived key. `consume(O, opId)`
uses O's `apply_archived_result` to validate each original report and issue its
exact ACK, including an older valid predecessor of newer direct evidence.
Older evidence never replaces O's current state. O and DB share the same strict
complete-snapshot validator in `graph_evidence.py`; the collector also compares
against retained intermediate versions to reject reordered conflicts.

The catalog is explicit: `unregister` refuses while archived versions remain.
Diagnostics use a separate oldest-first ring and carry no completion/closure
authority. Archive retrieval and O ACK are trusted local Python APIs, not an
additional unauthenticated network protocol. Like the existing research port,
UDP peer attribution is configured-address attribution, not cryptographic proof.
DB process failure loses its RAM copies, potentially including the only remaining
history after device retirement; no persistence or replication is claimed.

Example, given an O-authorized `request` and separately enrolled DB transport:

```python
db = GraphCollector(db_transport, authority=99)
db.register(request)
# O submits request through its own existing transport.
db.collect(request.operation, allowance_us=500_000)
# O may have been unreachable while DB collected this version.
db.consume(o, request.operation)
```

For direct handoff, O obtains the report with `query_report`, incorporates it with
`apply_result`, and passes the returned non-None ACK to `transport.acknowledge`.
The existing convenience `query(o, ...)` still only incorporates evidence; it
does not silently promise archival storage or emit a storage ACK.

## Verification and limits

New deterministic tests cover eligible oldest-version eviction, ACK preference,
atomic rejected admission, Unresolved/optional protection, obsolete ACKs, age
churn, sparse Ki work, incarnation exhaustion, cross-slot callbacks and final
response loss across retirement. Real UDP/WAMR tests exercise live ACKs, later
closure, DB full, DB loss and O socket loss with subsequent archive consumption.
New Python tests exercise exact-version retention, direct/archive reordering,
attribution, conflicts, bounded storage and independent diagnostic eviction.

The new four-node fixture runs three full lifecycles with four WAMR nodes,
16 bindings, 32 occupied queues and all 24 operation slots. It acknowledges live
reports, removes all nodes through overlapping full removals, preserves newer
creator evidence and reclaims under pressure without test-side forget calls.
Existing tests remain byte-for-byte unchanged from the P7 session baseline.
Results and source hashes are recorded in
[p7_verification.json](../verification/graph_core/p7_verification.json).

The 32-bit ESP32 compiler reports these four-node structure sizes:

| Structure | Before P7 | P7 | Change |
|---|---:|---:|---:|
| Admission pool (includes records) | 36392 | 36776 | +384 |
| One admission record (included above) | 1184 | 1200 | +16 |
| Node service | 184 | 184 | 0 |
| Edge service | 2704 | 2896 | +192 |
| Control adapter | 6848 | 7120 | +272 |

The aggregate additional static storage across pool/services/control is 848
bytes. This is not total device RAM.

The ESP32 P7 fixture was rebuilt, flashed and passed on the connected ESP32
revision 3.0 on 12 October 2026. The unchanged fixture completed all three
four-node lifecycles and its final report-reclamation assertions. At all queues
full, internal 8-bit heap free/minimum was 40340 bytes and the largest free block
was 18432 bytes. After each cycle, free heap returned to 153268 bytes and the
largest block to 110592 bytes: no progressive loss was observed across these
three cycles. The sampled main/control task stack high-water margin was 6908
bytes; the smallest sampled worker margin was 5888 bytes. These are measurements
from this bounded local workload, not a Wi-Fi or long-duration fragmentation
qualification. The fixture does not instantiate the UDP control adapter or a
networked collector. See the [serial log](../verification/graph_core/results/p7/board.log)
and the firmware/configuration hashes in the verification record.

Distributed Wi-Fi/collector target qualification and final implementation
TLA+/TLC checks remain P9 work; ventilation application decisions remain P8 work.

Q05 characterization is separate from conformance: the new test retires a
FailedClean addition, replays its old authorization and observes node reservation
again. Discarded rejection history can permit obsolete construction, resource
consumption and, if execution proceeds, guest/peripheral/data effects. This
slice adds no permanent rejection store, fresh-confirmation protocol or automatic
O sweep. Current missing history proves neither absence nor historical failure.

Two review findings were corrected with new regressions: equal incarnations in
different slots must not match an old effect, and final owner replies must remain
replayable across coordinator report retirement. The final focused review found
no remaining defect in those fixes. Full regression additionally caught premature
Ki fence retirement; keeping settled Ki records until admission pressure preserves
the unchanged P6 delayed-construction test. Generic implementation-skill advice was used
for incremental verification and two-axis review; no ticket framework, interface
hierarchy or automatic commit of pre-existing workspace changes was introduced.
