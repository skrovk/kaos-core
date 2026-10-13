# P4 endpoint service and edge operations

P4 implements edge addition and removal between established nodes, copied
best-effort data, and channel-state notifications. It uses the same portable C
core on Linux and ESP32. Multi-channel node construction and full distributed
node removal remain P5 and P6.

## Architecture and ownership

`kaos_admission.c` owns identity, reservations and removal fences. Admission
reserves an endpoint role and a notification slot on its local node atomically.
`kaos_endpoint.c` owns a fixed FIFO for each endpoint role. Preparation charges
its already-reserved payload storage and initializes queue metadata; it does
not call a guest, allocate memory, wait for a peer, or open data permission.
Its bounded step runs independently of runtime preparation.

`kaos_edge.c` owns operation decisions and fixed deadlines. Each record has
separate local owner facts and accepted aggregate facts. This distinction is
essential: preparing a receiver or receiving a command acknowledgement does
not establish that the coordinator accepted receiver ACTIVE. Local observations
use the same guards as remote owner reports.

`runtime/kaos_wamr.c` validates guest offsets and supplies trusted caller-slot
attribution. The runtime calls bounded channel callbacks without holding its
mailbox mutex. The port takes a short registry lock for admission, closure and
copies. No guest pointer survives a callback. Runtime cancellation and disposal
continue to use the P3 worker mailbox and lifetime tickets.

`ports/p4_host.inc`, included by the shared host port for `kaos_p4_host`, connects
these components to UDP. It keeps one confirmable management exchange per
remote peer and transfers bounded batches of data between timer/control work.
Messages sent outside the registry lock own their bytes and retain exact
binding identity. A stalled peer does not occupy another peer's exchange slot.

Every local channel has **two separate queues**: outgoing and incoming. A
single shared queue was rejected because it would make a full receiver cause
local `put` to fail, whereas the remote contract permits local acceptance and
subsequent receiver-side loss. Separate queues match existing per-role
reservations and avoid shared-allocation ownership and reference counting.

## Edge lifecycle

For `A -> B`, the source owner is the coordinator:

1. Reserve the immutable scope and independently prepare both endpoint roles.
2. Accept both READY observations before recording COMMIT. Neither role has
   data permission yet.
3. Activate B's receiver. B can now receive and `get`.
4. Accept B's cumulative ACTIVE report, then activate A's sender. A can now
   `put` and send.
5. Accept both ACTIVE facts before the original addition deadline, then retain
   historical Succeeded.

Failure, cancellation or expiry selects cleanup for both scoped endpoints,
including a remote role whose preparation was never observed. A cleanup fence
can arrive before preparation. Removal has a fresh operation ID, its own fixed
deadline and closure observations; it does not reuse the creator's ACTIVE
history. Neither operation destroys a peer runtime.

Closure blocks new access immediately. Queued data is discarded, outstanding
accesses must return, payload charges and endpoint reservations are released,
and only then is CLEANED observed. At cutoff, unresolved obligations produce
Unresolved. Coordinator cleanup retries stop; retained owner reports may still
deliver late CLEANED and finish the operation. Retries never reset deadlines
or begin a new exchange wave for unchanged evidence.

An unexpected node exit closes its incident local binding resources. For a
co-located binding, both local roles close so that one cannot pin the other's
reservation. The peer node keeps running. After historical edge success this
does not invent distributed removal: Q12 still requires fresh O authorization
for that operation. Ports run a second bounded node step after endpoint cleanup
so registry release does not require another runtime wakeup.

## Data and guest ABI

Imports use module `kaos`. A logical channel is 24 bytes: three little-endian
`u64`s `(sourceLogical, destinationLogical, service)`. Guests never select a
concrete binding generation, and one call never fans out.

| Import | Result |
|---|---|
| `put(channel_offset, data_offset, size)` | 0 after copying one complete message into outgoing storage |
| `get(channel_offset, data_offset, capacity)` | Copied byte count, including 0 for a zero-length message |
| `event_get(data_offset, capacity)` | 0 after copying and consuming one channel-state notification |
| `event_wait(timeout_us)` | 0 if a notification is pending, 1 on timeout, -1 on cancellation/invalid timeout |

The data and event calls return -1 DISABLED, -2 FULL, -3 EMPTY, -4 INVALID, or
-5 SIZE as appropriate. Invalid or undersized reads leave the message queued.
The runtime checks guest ranges without turning an ordinary invalid argument
into a WAMR trap. Data calls are permitted only during authorized main execution.
P3 `init` and `error` behavior is unchanged.

Each FIFO has up to four messages of at most 256 bytes, further limited by its
channel descriptor. `put` success establishes local buffer acceptance only.
Each asynchronous transfer consumes one outgoing message for one attempt;
receiver overflow drops that incoming message and preserves queued messages.
Local delivery calls the same receive function as decoded remote traffic.
There is no channel retry, acknowledgement, deduplication, freshness timer or
network ordering guarantee. FULL, EMPTY and ordinary loss do not fail an edge.

Notifications contain the logical triple, little-endian `u32` permission bits
(PUT=1, GET=2), and four reserved zero bytes: 32 bytes total. State changes
coalesce to the latest pending value per channel. A slot remains reserved while
the endpoint exists, even after reading an event. After removal only an unread
event remains; endpoint memory is released independently. Consuming that event
frees the slot. Admission refuses another distinct channel when active plus
unread removed-channel notifications exhaust the node's bound. Re-addition of
the same logical channel can coalesce into its slot; exact creation identity
guards prevent old callbacks from overwriting replacement state.

Finite event waits use a retained wake flag and recheck readiness across wait
registration. They release both registry and mailbox locks while waiting.
Only the calling runtime waits; cancellation still interrupts it.

## Wire contract: schema 4

This is a coordinated upgrade from schema 3. Existing request/report shapes
and operation meanings remain unchanged; new activation commands and channel
ABI require the shared version bump. The simulator encoder and the explicitly
approved version literals in existing tests were updated together. Each build
accepts one version only.

Management uses the existing unsecured CoAP/UDP and deterministic CBOR adapter:

| Resource | Body and meaning |
|---|---|
| `/prepare` | Original edge addition/removal request; independently admit the local scope |
| `/cancel` | Original edge addition request; install a cleanup fence |
| `/activate` | `[4,11,opId,objectIndex]`; activate the prepared local role |
| `/owner` | `[4,10,opId,owner,objectIndex,cumulativeFacts]` |
| `/query` | Existing query; return the retained coordinator aggregate |

Transport peer attribution must match the enrolled owner and immutable scope.
Response codes acknowledge receipt only. Cleanup responses may additionally
carry an explicit owner-report body, accepted through the same evidence guards.
Repeating a cleanup command can therefore retrieve later CLEANED after a lost
report. Unsegmented edge-removal commands bypass the ordinary response cache,
so a duplicate cannot be stuck replaying an earlier CLEANUP-only snapshot.
This creates no new retry wave. Owner reports are retained facts
sent in bounded confirmable exchanges, including after a coordinator cutoff.
Conflicting, malformed or unauthorized owner evidence cannot advance an
operation. Each edge effect fits a 160-byte owned body.

Application data uses one UDP datagram, with no CoAP acknowledgement. Its
52-byte header is: schema `u8`, tag `u8` (=1), six network-order `u64`s (logical
source, logical destination, service, creating operation, source instance,
destination instance), then payload length `u16`. Exactly that many payload
bytes follow. The maximum frame is 308 bytes. Truncated, oversized, mismatched
owner/instance/binding, or disabled-receiver frames are rejected. An old
datagram cannot be redirected to a replacement binding.

## Profiles and qualification

The default profile remains two nodes, eight endpoint roles, four operation
records, and four incident channels per node. `CORE_FOUR_NODE_PROFILE` selects
four nodes, 32 endpoint roles, 24 operation records, eight incident channels
and eight ports per node. Its complete report scope is 17 objects; the
obligation mask is 32 bits and the maximum revision is 142. Limits are static,
without negotiation or runtime discovery. Larger full-node operations still
need their P5/P6 functional qualification.

The four-node fixture uses a ring with four distinct services per directed
link: 16 channels, four incoming plus four outgoing endpoints per node. All
nodes run actual WAMR guests and use production admission, node, edge and data
services. It then removes the edges while the guests keep running and finally
removes the nodes. Artifacts are staged directly by the fixture; it does not
claim Wi-Fi capacity, collector behavior, arbitrary application arena sizes,
or completed P9 target qualification.

Focused tests are `endpoint_test.c`, `edge_test.c`, `wamr_channel_test.c` and
`p4_process_test.py`. They exercise HP03–HP06, PERM permission/FIFO/loss rules,
exact binding identities, coalesced event capacity, receiver evidence ordering,
cleanup before preparation, tracked access at cutoff, late evidence, and peer
runtime preservation. The process cases use the real Python authority, CoAP,
artifact transfer, C control and WAMR workers for local and remote placements.
The existing tests retain their scenarios and assertions.

Final queue/worker/timer TLA+ composition and distributed Wi-Fi qualification
remain P9 work; earlier model results are not presented as new P4 verification.

### Measured local target profile, 10 October 2026

The physical ESP32 without PSRAM passed three complete cycles with four WAMR
nodes, eight endpoints per node, all 32 queues full at four 256-byte messages,
validated guest traffic, edge removal with live peer runtimes and subsequent
node reclamation. The fixture uses 26,624-byte node arenas and 8,192-byte native
worker stacks, with control priority 3 above worker priority 2. A first board
run exposed the new fixture's incorrect default control priority; matching
the P3 scheduling profile corrected teardown starvation without changing guest
scenarios, assertions or memory limits.

Free byte-addressable heap is 39,588 bytes with full queues, with an 18,432-byte
largest block. Cleanup restores 152,516 bytes free and a 110,592-byte largest
block in each cycle. Node arenas peak at 25,472 bytes; the 32 KiB global arena
uses only 40 bytes throughout the run. These are fixture measurements, not
universal minimum sizes or a Wi-Fi-capacity claim. The
[memory breakdown](graph_p4_memory.md) separates the fixed pools, actual arena
consumption, task stacks and unmeasured networking costs, and records candidate
cuts without applying them.

All 20 host CTest entries pass with the selected sanitizer configuration,
including the new real Python/UDP/WAMR local and remote edge cases and the
existing Python regression discovery. The separate four-node host capacity
fixture also passes. Commands, hashes, preserved-test checks and limits are in
the [P4 verification record](../verification/graph_core/p4_verification.json).
