# P4 ESP32 memory measurements

Measured on 10 October 2026 on the enrolled ESP32 T-Display, ESP32-D0WDQ6-V3,
without PSRAM, using ESP-IDF 5.5 and the pinned WAMR interpreter. All sizes are
bytes unless marked KiB (1024 bytes). This is the four-node qualification
profile, not the smaller default profile.

**Four actual WAMR nodes with eight endpoints each fit and run.** Three complete
cycles filled all 32 queues, exchanged validated 256-byte messages, removed the
edges while nodes remained alive, then removed the nodes and reclaimed their
memory. Each cycle restored the same free heap. The fixture has no Wi-Fi,
network stack, sockets, or instantiated management transport; those costs remain
to be measured before claiming the complete networked product fits.

No additional memory cuts were made during this measurement. The measured
configuration is 26 KiB per node arena, 8 KiB per native worker stack, and the
existing 32 KiB global arena. Trace counters add 80 bytes of static RAM.

## Working data RAM

At the full-queue snapshot, the following accounting reconciles exactly to the
331,952 bytes of static DRAM plus raw byte-addressable heap available to this
image. Reserved capacity is counted even when empty or not yet touched.

| Allocation or free capacity | Bytes | KiB |
|---|---:|---:|
| Admission: nodes, endpoint identities and operation records | 39,272 | 38.35 |
| Endpoint FIFOs and service metadata | 33,232 | 32.45 |
| Edge and node state-machine services | 1,352 | 1.32 |
| WAMR global arena | 32,768 | 32.00 |
| WAMR service, allocator records and other runtime globals | 1,312 | 1.28 |
| Fixture bookkeeping and synchronization handles/storage | 444 | 0.43 |
| ESP-IDF/libc globals and linker padding | 11,500 | 11.23 |
| **Static DRAM subtotal** | **119,880** | **117.07** |
| Four node arenas, 26,624 each | 106,496 | 104.00 |
| Four retained WASM artifacts, 1,124 each | 4,496 | 4.39 |
| Four native worker stacks, 8,192 each | 32,768 | 32.00 |
| Main/control task stack | 12,800 | 12.50 |
| Two idle and two IPC task stacks | 5,120 | 5.00 |
| Nine task-control blocks, 348 each | 3,132 | 3.06 |
| Other dynamic platform costs and heap bookkeeping, by subtraction | 7,672 | 7.49 |
| **Occupied/reserved heap subtotal** | **172,484** | **168.44** |
| **Free byte-addressable heap** | **39,588** | **38.66** |
| **Total accounted data RAM** | **331,952** | **324.17** |

The 7,672-byte remainder is deliberately not assigned to a guessed structure.
It includes pthread/platform allocations, lazy synchronization and allocator
bookkeeping not individually traced. The largest free block is **18,432 bytes
(18 KiB)**, so 38.66 KiB free does not mean another 26 KiB arena can be allocated.
Both total capacity and contiguous block size matter.

The configured main stack is 12,288 bytes; ESP-IDF adds 512 bytes for the selected
libc configuration. Two 1,536-byte interrupt stacks are already in static DRAM.
Configured timer, event and TCP/IP task sizes are not added: those tasks do not
exist in this image. The physical task listing contains precisely these nine
tasks.

## What is inside the fixed core pools?

The admission object is 39,272 bytes:

| Part | Count × size | Total |
|---|---:|---:|
| Enrollment, quota and header fields, including padding | 1 × 104 | 104 |
| Live node records | 4 × 496 | 1,984 |
| Live endpoint records | 32 × 184 | 5,888 |
| Retained operation records | 24 × 1,304 | 31,296 |

Each node record includes eight 40-byte notification slots: 1,280 bytes across
the four nodes. Unread removal notifications can survive endpoint reclamation;
they do not retain the endpoint's payload allocation.

An operation record retains identity/deadline fields, a 40-byte aggregate
report, 17 bytes of separate owner facts, a 136-byte node request, a 944-byte
immutable scope, flags/padding, and an **unused 128-byte `report[]` array**.
The scope contains eight 88-byte bindings, eight manifest ports and a predicate
buffer. P4 single-edge operations need only one binding, but every record
reserves the maximum space needed by a future multi-channel node operation.
The measured topology retains 20 creator records (four nodes plus 16 edges),
leaving four slots for removals or other operations.

Current endpoint identities and historical operation scopes have different
lifetimes. Replacing the immutable scope with a pointer into a reusable endpoint
slot would lose historical identity and authorization information. Compact
storage is possible, but it needs more than a struct-field deletion. A union
whose largest variant is still the full node scope saves no fixed-pool memory.

The endpoint service is 33,232 bytes:

- 32 × 4 × 256 = **32,768 bytes of payload storage**.
- 32 × 14 = 448 bytes of queue lengths, cursors, flags and padding.
- 16 bytes of service pointers and cursor.

A local channel has a sender queue and a receiver queue. This permits a
successful local `put` followed by receiver overflow/drop, matching remote
best-effort semantics. There are 16 channels in this fixture, hence 32 endpoint
queues. The 1,024-byte admission charge per endpoint refers to this existing
static payload storage; it is not a second heap allocation.

## One node's runtime memory

Each node reserves **26,624 bytes of arena backing**, plus an **8,192-byte native
worker stack** and a **1,124-byte artifact copy** outside the arena. Those three
allocations total **35,940 bytes (35.10 KiB)**, before task/platform metadata and
the separately counted fixed core pools. Artifact size alone is therefore a
poor estimate of the cost of a runnable node.

The arena is a bounded bump allocator: individual WAMR frees do not return
space; all backing storage is returned when the node is disposed. Its final
used counter is therefore a whole-lifecycle allocation peak, including loader
temporaries, rather than just the amount of live guest data.

| Inside one node arena | Bytes |
|---|---:|
| Guest linear memory backing | 12,288 |
| WAMR interpreter stack | 8,192 |
| Execution environment and cluster metadata | 1,673 |
| Module loader payload, including retained temporary allocations | 1,646 |
| Instance metadata | 908 |
| Extra disposal traversal allocation | 4 |
| Allocation headers | 704 |
| Alignment padding | 57 |
| **Whole-lifecycle arena usage** | **25,472** |
| **Unused arena capacity** | **1,152** |
| **Arena capacity** | **26,624** |

The 12,288-byte linear allocation contains a 1,792-byte guest data/address
region, a 4,096-byte guest C stack, a 4,096-byte WAMR-managed application heap,
and 2,304 bytes of page rounding. The data region includes the linker's leading
address gap; it is not all initialized application data. The application heap
holds the configuration copy as well as its internal allocator metadata.
These numbers come from the exported layout (`data_end=1792`, `heap_base=5888`)
and the pinned WAMR allocation path. None should be added again to arena usage.

Preparation uses 25,448 bytes. Disposal adds 24 bytes including its allocation
header and alignment. All 12 measured node lifetimes reached the same
25,472-byte peak. The remaining 1,152 bytes are not a universal safety margin:
larger modules or different call depth can require more memory. This run does
not measure the interpreter stack or application heap's internal high-water
marks and does not establish the smallest viable arena for arbitrary guests.

Native stack high-water measurements, including preparation and cleanup, left
5,888 / 6,004 / 6,008 / 6,012 bytes free in the four 8,192-byte stacks. Maximum
observed worker stack use was therefore 2,304 bytes. The main stack left 6,908
bytes free at the full-queue snapshot. These workload observations suggest
possible future reductions; they are not worst-case guarantees.

The **global** arena is particularly different: initialization, pre-destroy and
post-destroy all report **40 bytes used out of 32,768**. It contains one 20-byte
allocation, a 16-byte header and four bytes of alignment. All node allocations
are charged to their owning worker's separate arena. This is the strongest
current candidate for reducing a reservation without changing queue capacity.

## Heap over the lifecycle

| Phase | Free data heap | Largest block |
|---|---:|---:|
| Before WAMR initialization | 188,016 | 110,592 |
| Four persistent workers ready | 152,628 | 110,592 |
| After first registry lock, before nodes | 152,516 | 110,592 |
| Four running nodes, all 32 queues full | 39,588 | 18,432 |
| After removing all nodes, workers retained | 152,516 | 110,592 |

The last two rows repeated identically for all three cycles. Each cycle
delivered 1,000 messages and observed guest validation on every node; queue
FULL and best-effort loss did not fail the operations. Endpoint payload
reservations were released even while 31–32 removal notifications remained
unread. Node removal then reclaimed the remaining reservations.

An earlier diagnostic reached traffic but suffered control-task starvation
during removal. P4's new board entry point had retained the default priority 1
while workers ran at priority 2. It now matches P3: control priority 3, workers
priority 2. No guest scenario, assertion or memory limit was changed to fix
that defect. The final run completed without watchdog reports.

## Where the rest of the advertised 520 KiB goes

Not all ESP32 SRAM is interchangeable byte-addressable data memory. The current
image reconciles the main SRAM banks as follows, without counting D/IRAM aliases
twice:

| Physical use or region | Bytes | KiB |
|---|---:|---:|
| CPU flash caches | 65,536 | 64.00 |
| ROM reservations | 3,920 | 3.83 |
| Application static DRAM | 119,880 | 117.07 |
| Loaded instruction RAM, including alignment | 55,096 | 53.80 |
| Raw byte-addressable heap regions | 212,072 | 207.10 |
| Raw instruction-only heap region | 75,976 | 74.20 |
| **Main SRAM total** | **532,480** | **520.00** |

The 75,976-byte instruction-only region cannot satisfy the current normal
byte-buffer arena allocation. `MALLOC_CAP_EXEC` also covers dual-use regions,
so its free counter overlaps the data-heap counter and cannot be added to it.
RTC RAM is separate and is not enabled as ordinary heap here.

The firmware binary occupies **309,744 bytes of flash**, leaving 738,832 bytes
in its 1 MiB application partition. Most WAMR and core code remains mapped
from flash. Flash capacity is not the current constraint; moving numbers from
the flash column into a RAM total would misrepresent working memory.

## Candidate cuts, in recommended order

These are proposals, not implemented or newly qualified configurations.
Savings can overlap; do not sum alternatives blindly.

| Candidate | Potential saving | Consequence / needed check |
|---|---:|---|
| Global arena 32 KiB → 1 KiB | **31,744 B (31 KiB)** | Leaves 984 B above observed usage; qualify all runtime init/failure/shutdown paths before adopting. |
| Remove unused `record.report[128]` | **3,072 B (3 KiB)** | No selected capacity reduction; retain the actual aggregate evidence and run regression checks. |
| Native worker stacks 8 KiB → 6 KiB | **8,192 B (8 KiB)** | Current high-water data is encouraging; qualify deeper calls, errors and cancellation before reducing. |
| Queue depth 4 → 2 | **16,512 B (16.13 KiB)** | Same nodes/endpoints/payload size; less burst capacity, earlier FULL and incoming drops. Includes length-array savings. |
| Maximum payload 256 → 64 bytes | **24,576 B (24 KiB)** | Same topology and depth; changes supported workloads and manifests. |
| Operation slots 24 → 22 | **2,704 B (2.64 KiB)** | Only two slots beyond the 20 live creators; less concurrent work/history capacity. Includes engine-table savings. |
| Separate compact edge-operation storage | About **14 KiB** for 16 retained edge creators, before extra pool bookkeeping | More ownership/layout complexity; preserve historical scopes and consider P5/P6 together. |

The first two changes could recover **34 KiB** without reducing the four-node,
eight-endpoint, four-message, 256-byte profile. These were the initial candidates
from the measurement; the follow-up proposal below also considers smaller
application messages. Further shrinking the node arenas has at most 4,608 bytes of
aggregate observed slack across all four nodes and sacrifices a small existing
margin, so it should not be the first target.

We have demonstrated the requested four-node local baseline, not exhausted a
fundamental four-node hardware limit. We have also not demonstrated the
networked product's full memory budget. Concrete Xtensa layouts give a useful
warning about the remaining work: one `core_control` object is 6,848 bytes;
each outgoing exchange plus owned edge effect is another 1,408 bytes. Eight
such exchange slots plus control would total **18,112 bytes**, before peer
addresses, artifact transfers, sockets, network tasks, driver buffers and
Wi-Fi. This is a possible port subtotal, not a chosen or measured embedded
network design. The next capacity gate should measure a minimal real Wi-Fi
and management port after the low-complexity cuts are selected.

## Follow-up proposal and next steps — 10 October 2026

Recorded after the measurement snapshot, at the user's request. This is a
configuration to evaluate, not an implemented or qualified change:

- Keep four nodes and eight endpoints per node.
- Reduce maximum message payload from 256 to **128 bytes**.
- Keep **four messages per endpoint**, preserving the current burst capacity.
- Reduce the WAMR global arena from 32 KiB to **1 KiB**.
- Keep the measured 26 KiB node arenas and 8 KiB native worker stacks initially.

Payload storage would fall from 32 KiB to 16 KiB. Together with the smaller
global arena, the projected saving is **47 KiB per device**. This excludes the
separate potential 3 KiB saving from removing the unused operation report array.
Measurements and short commands are expected to fit 128 bytes, but actual
encoded application messages must confirm that assumption. Oversized messages
must be rejected, not truncated.

An alternative is to route WAMR's runtime-wide allocations directly to the
ESP32 heap while retaining bounded per-node arenas. That avoids a fixed global
reserve but makes those allocations compete with other platform services and
requires correct allocation/free routing, alignment and failure handling. The
initial preference is the 1 KiB fixed arena: predictable capacity, minimal
interface change and nearly the same saving. Direct global-heap allocation
remains an option, not a selected implementation.

Next steps, in order:

1. Check representative Beacon/application message encodings against 128 bytes
   and qualify their actual module/runtime requirements; artifact size alone
   is insufficient.
2. Implement the candidate profile consistently across admission, manifests,
   guest/data bounds and authority-side validation. Add boundary cases for a
   128-byte message and rejection above the limit. Preserve existing tests;
   seek approval for any needed changes beyond the previously authorized
   schema-version literals.
3. Re-run host checks and physical four-node/eight-endpoint qualification with
   full queues, runtime failure/cancellation paths and repeated cleanup. Record
   global-arena peaks, stack watermarks, minimum free heap and largest blocks.
4. Add the smallest real ESP32 Wi-Fi/management/data port and repeat under
   traffic, loss and concurrent cleanup. Measure actual network allocations
   and size exchange capacity to demonstrated needs rather than assuming eight
   peer exchanges are necessary.
5. Record the supported networked profile and remaining P4 acceptance gaps.
   Continue with P5 multi-channel node addition once those gates are resolved;
   retain final composed formal verification and distributed qualification in
   the implementation plan.

## Evidence and reproduction

- [Physical run](../verification/graph_core/results/p4/board-memory.log):
  all three cycles, arena counters, task watermarks and boot regions.
- [Core layouts](../verification/graph_core/results/p4/core-memory.txt) and
  [target compiler probe](../verification/graph_core/results/p4/core-layout.c).
- [Runtime accounting](../verification/graph_core/results/p4/runtime-memory.txt).
- [Platform/archive breakdown](../verification/graph_core/results/p4/platform-memory.txt).
- [Verification manifest](../verification/graph_core/p4_verification.json):
  source/build hashes, commands, passed checks and unqualified boundaries.

Build with `bash verification/graph_core/espidf_p4/build.sh`; it selects
`CORE_FOUR_NODE_PROFILE` and trace-only memory counters. Flash from
`/tmp/kaos-p4-session/esp32` using ESP-IDF's generated `flash_args`, then run
`verification/graph_core/read_serial.py` with the ESP-IDF Python environment.
The serial reader resets `/dev/ttyUSB0` and captures a bounded qualification
run. The capacity guest is built from `verification/graph_core/guests/capacity.c`.
Its 1,124-byte artifact and measured arena requirement are specific to this
workload, not a replacement for measuring the Beacon application guests.

## P5 implementation follow-up — 11 October 2026

P5 removed the unused `record.report[128]` storage, retaining the actual
`core_report` evidence. Its raw saving is 3,072 bytes for 24 records, partly
offset by the new bounded per-channel cleanup/evidence bookkeeping. The
128-byte payload and 1 KiB WAMR global-arena proposals above remain unqualified
and were not applied. The [P5 contract](graph_p5_contract.md) and verification
record distinguish host/cross-build evidence from the earlier physical
measurements; no board was connected for a new measurement run.
