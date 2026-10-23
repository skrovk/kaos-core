# Using the KaOS graph system

Use the isolated graph targets for the six-operation implementation.
`kaos_host` supports all six operations, reporting and collector integration.
Commands below start in the `kaos_core` repository root unless stated otherwise.

## Build the host

Prerequisites: Linux x86_64, a C compiler, CMake 3.18 or newer, Ninja, Python
and `uv`; the sibling `../sim_env` checkout with its dependencies available
offline; WASI SDK clang at `/opt/wasi-sdk/bin/clang`; and WAMR at
`../sim_env/.deps/wamr`, pinned to
`b124f70345d712bead5c0c2393acb2dc583511de`.

```sh
cmake -S verification/graph_core -B /tmp/kaos-guide-host -G Ninja \
  -DCORE_BUILD_HOST=ON -DCORE_BUILD_P2_HOST=ON -DCORE_SANITIZE=ON
cmake --build /tmp/kaos-guide-host -j4
```

Set `CORE_WAMR_ROOT` and `CORE_WASI_CLANG` at configuration if those dependencies
are elsewhere. The build applies reviewed corrections to copies of pinned WAMR
sources; a different revision requires review. The standalone core library can
also build from `graph_core` without WAMR. `CORE_BUILD_HOST` builds the complete
host. `CORE_BUILD_P2_HOST` adds only the historical artifact-only verification
fixture needed by sibling Python regressions. The old P3/P4 build options and
executable aliases have been removed; use
`CORE_BUILD_HOST` and `kaos_host`. Historical evidence retains the original names.

## Run the six-operation experiment

After the host build, run this command from `kaos_core`. The output directory
must be new; existing evidence is never overwritten.

```sh
uv --directory ../sim_env run --offline python -m sim_env.graph_run \
  --host /tmp/kaos-guide-host/core/kaos_host \
  --guest /tmp/kaos-guide-host/traffic.wasm \
  --output /tmp/kaos-graph-example --seed 42
```

The example starts two hosts and executes these explicitly authorized steps:

1. Add source node 10 on device 1 without channels.
2. Add receiver node 20 on device 2 with a required source→receiver channel.
3. Remove that channel, preserving both nodes.
4. Add a fresh binding for the same logical channel.
5. Remove receiver node 20 and its complete current incidence.
6. Remove source node 10 without channels.

`result.json` contains the accepted reports for all six operation kinds.
`events.jsonl` retains immutable submissions, admission responses, accepted
reports, exact storage ACKs, host stdout, configured link changes, packet
decisions/delivery and process exits. `provenance.json` records the executed
binary and Python source hashes, participant addresses and seed. Artifact
registration records its SHA-256. These are host workload budgets, not target
capacity or timing claims.

An unintended relay queue overflow now fails run completion. The final teardown
record contains `relay_overflow_drops` and an error; raw observations remain
available, but the built-in runner does not write its normal `result.json`.
Configured link loss remains valid impairment. A zero overflow count does not
establish complete observation coverage or absence of kernel receive loss.

For a custom Python experiment, `GraphRun(host, output, devices=(1, 2))` is a
context manager. Register bytes with `run.add_artifact(reference, bytes)`,
authorize through `run.o`, and use `run.submit(request)`, `run.query(request)`
or `run.wait(request, timeout_s=5)`. Existing `Node`, `Channel` and `Port`
descriptors define the workload; there is no second scenario language.
`submit()` returns admission transport evidence. It does not call
`o.rejected()` automatically or treat a transport timeout as rejection.

For example, inside a run:

```python
# Drop every datagram from device 1 to device 2, including control and data.
run.network.set_link(1, 2, loss=1)
# The reverse direction may have a different policy: loss is asymmetric.
run.network.set_link(2, 1, delay_ms=20, duplicate=0.25)
# Restore the forward direction. This does not alter the reverse direction.
run.network.set_link(1, 2)
```

All packet types share the policy on their directed link. To model a complete
bidirectional outage, set `loss=1` on both `(1, 2)` and `(2, 1)`.
Changing a delay affects new arrivals; queued packets retain their due times,
allowing scripted reordering. Setting loss to 1 also drops queued packets at
delivery. Independent operations can use calls from separate Python threads;
one exchange per coordinator remains enforced. Use the returned report's
outcome, not packet delivery or diagnostic markers, to assess graph completion.

To inject a device crash, call `run.crash(2)` inside the context. It sends SIGKILL
to the managed host and returns after confirming its exit. All nodes, endpoints
and coordinator roles in that process stop together. The runner records
`device_crash_requested` and `device_crashed`, and permits the surviving hosts
to continue. Unknown devices and already-exited hosts are rejected. There is
no restart operation. An unrequested process exit still fails the run; stderr,
reader failures and relay overflow remain errors even after an intentional crash.

Process death supplies no graph closure evidence. Queries to that coordinator
can time out, and remote cleanup can remain Unresolved. O retains possible
incidence until attributed reports establish closure. A killed host may leave
no `CHANNEL_END`; its trace is incomplete. Previously buffered stdout can be
logged after the crash record, so log arrival order is not an execution barrier.
The [shared-host crash check](verification.md#shared-host-crash) demonstrates this.

## Run pipeline and feedback workloads

The host build also produces `pipeline.wasm` and `feedback.wasm`. From the
repository root, use a new output directory for each experiment:

```sh
uv --directory ../sim_env run --offline python -m sim_env.graph_workloads \
  --host /tmp/kaos-guide-host/core/kaos_host \
  --guest /tmp/kaos-guide-host/pipeline.wasm --workload pipeline \
  --output /tmp/kaos-pipeline-example --seed 42

uv --directory ../sim_env run --offline python -m sim_env.graph_workloads \
  --host /tmp/kaos-guide-host/core/kaos_host \
  --guest /tmp/kaos-guide-host/feedback.wasm --workload feedback \
  --loss 0.25 --delay-ms 5 --output /tmp/kaos-feedback-example --seed 42
```

Pipeline uses three devices, removes its transform, adds a fresh transform and
checks a second observation window before explicitly removing all nodes.
Feedback uses two devices and one outstanding request. Its example delays
authority→initiator preparation, then applies loss/delay on device 1→2 during
the observation window. Reverse traffic retains its default policy. All packet
types on each configured link share that policy; the runner restores the link
before requesting cleanup.

Add `--local` for co-located execution. Feedback fits the default two-node
profile. Pipeline needs the [four-node build](verification.md#run-the-checks);
use `/tmp/kaos-guide-four/core/kaos_host` and its `pipeline.wasm`. Local runs
reject nonzero network impairment options because local channels bypass UDP.
The local pipeline removes its transform but leaves source/sink for process
teardown: its three additions plus removal fill the four-entry CoAP response
cache. `remaining_instances_at_teardown` records those two live instances.
Teardown is not protocol cleanup, and no cache bound is enlarged for the example.

`result.json` separates accepted graph reports from sample results. A passing
fixture requires a complete channel trace, correct values/padding and at least
eight distinct output samples in each 250 ms observation window. These are
synthetic test settings, not application timing or service requirements.
CLI exit codes are 0 for pass, 1 for failed/inconclusive sample evaluation, and
2 for an execution or validation error; inspect the retained events on errors.
See the [code walkthrough](workload-walkthrough.md) for architecture and examples.

## Replay a synthetic sample budget

Evaluate an existing workload trace without launching hosts or modifying its
original results:

```sh
uv --directory ../sim_env run --offline python -m sim_env.graph_workload_timing \
  /tmp/kaos-pipeline-example --budget-us 50000 \
  --output /tmp/kaos-pipeline-timing.json
```

The explicit budget is in host monotonic microseconds. It measures the first
successful source PUT to the first matching final GET, including intermediate
processing and retries. It is a synthetic publication budget, not acquisition
age or a protocol deadline. The same command works with feedback runs.

The report keeps `sample_status` separate from its timing `status`. A value
check or graph operation may pass while the timing budget fails. Eligible
publications must originate inside the observation window and have their
entire budget strictly before its exclusive end. Tail publications are counted
as excluded, even if delivered early; prewindow origins stay excluded after a
retry. Late or unobserved eligible publications miss the budget, without
attributing a cause. Missing trace records or no eligible publications yield
an inconclusive result.

`--output` is optional; stdout always receives the JSON report. Output files
must be new. Input and evaluator hashes identify the replay. Exit codes are
0 for a timing pass, 1 for failed/inconclusive timing, and 2 for invalid
input or an output error. A timing pass does not override the original workload
result. See [timing semantics](workload-walkthrough.md#offline-timing-budgets)
for boundaries and examples.

## Run alternative outgoing channels

The host build also produces `alternatives.wasm`. Run APP03 with A initially
available and B initially partitioned:

```sh
uv --directory ../sim_env run --offline python -m sim_env.graph_alternatives \
  --host /tmp/kaos-guide-host/core/kaos_host \
  --guest /tmp/kaos-guide-host/alternatives.wasm \
  --first A --seed 42 --output /tmp/kaos-alternatives-a
```

Repeat with `--first B` and a new output directory to reverse the roles.
The runner starts two sinks, then adds a source with the predicate `A OR B`.
It checks samples through the available branch, restores the other directed
link, and waits for fresh evidence that both channels are ACTIVE. It then
explicitly removes the initially working channel and checks continued samples
on the remaining one before removing all three nodes.

Each of the seven operations must report success. `result.json` retains the
source's initial and joined reports, all operation outcomes and three sample
windows. Each window requires at least eight distinct samples on every expected
active branch and no successful PUT/GET on an inactive branch. Incomplete traces
are inconclusive. These are synthetic fixture checks, not throughput guarantees.

The partition affects all packet types from device 1 to the initially unavailable
sink; the reverse direction retains its default policy. Recovery occurs within
the original channel allowance. A storage-acknowledged report can temporarily
disappear, so the join loop polls for new facts and never treats missing history
as activation. Exit codes are 0 for pass, 1 for failed/inconclusive sample checks,
and 2 for an execution or validation error. Events remain available after errors.
See the [walkthrough](workload-walkthrough.md#alternative-outgoing-channels)
for branch behavior and evidence boundaries.

## Run mixed input/output dependencies

The host build produces `mixed.wasm` for APP04:

```sh
uv --directory ../sim_env run --offline python -m sim_env.graph_mixed \
  --host /tmp/kaos-guide-host/core/kaos_host \
  --guest /tmp/kaos-guide-host/mixed.wasm \
  --first A --missing output --seed 42 --output /tmp/kaos-mixed-a-output
```

`--first A|B` selects the available input. `--missing output|inputs` selects
which required part initially prevents startup. Use a new output directory
for every run; the verification suite exercises all four combinations.
The transform addition uses `(input A OR input B) AND output`. The runner
requires positive Pending/READY evidence before restoring the missing path,
then successful startup with complete ACTIVE input and output endpoint pairs.

The first 250 ms window checks transformed output. The runner then explicitly
removes the required output; the second checks continued input reads and zero
output copies. `result.json` keeps `service_status` separate from the expected
experiment `status`: service fails after removal while the experiment passes.
All nine graph operations must succeed, including removal of every node.

As with other fixtures, missing trace records are inconclusive, GET means host
copy-out, and sample counts are synthetic checks. Exit codes are 0 for expected
behavior, 1 for failed/inconclusive checks, and 2 for execution or validation
errors. See the [walkthrough](workload-walkthrough.md#alternative-inputs-with-a-required-output).

## Add and remove a node

Additional native functions can be authorized with the optional
`Node(..., imports=("fixture_temperature_read",))` argument. Names resolve only
under `kaos_native`; standard `kaos` imports need no additional grant.
See [native imports](native-imports.md) for encoding, guest syntax, bounds and
the synthetic sensor's contract.

In one terminal, start a fresh device host:

```sh
ASAN_OPTIONS=detect_stack_use_after_return=0 \
  /tmp/kaos-guide-host/core/kaos_host 58831 58832 58830 1 99
```

Arguments are device management port, device application port, O's management/
artifact port, device ID and O ID. The host binds loopback and prints
`LISTENING 58831 58832`. Both local ports must be explicit and distinct.
Optional peers follow as `device_id:management_port:application_port` (loopback),
or `device_id@IPv4:management_port:application_port`; enroll each
participating device on its peers. O and the collector use management-only
addresses; for example, `KAOS_COLLECTOR=77@127.0.0.1:58840`. Old single-port
host commands are incompatible and must supply the application port.

The ESP-IDF host uses `CONFIG_KAOS_UDP_PORT` for management and
`CONFIG_KAOS_APPLICATION_UDP_PORT` for application traffic. Configure every
remote device with both ports in `CONFIG_KAOS_PEERS`; O and DB retain only
their management addresses.

From `../sim_env`, run this example. It serves the generated lifecycle guest,
waits for accepted startup, removes the exact instance and acknowledges the
stored removal result. The budgets and waits are host demonstration settings.

```sh
uv run --offline python - <<'PY'
from pathlib import Path
from time import monotonic, sleep

import cbor2
from sim_env.graph_protocol import Node
from sim_env.graph_transport import GraphTransport
from sim_env.orchestrator import Orchestrator

o = Orchestrator((1,))
with GraphTransport(("127.0.0.1", 58830), {1: ("127.0.0.1", 58831)}) as transport:
    def complete(request):
        reply = transport.submit(o, request.operation, allowance_us=1_000_000)
        if reply.code != 65:  # CoAP 2.01: admission, not operation success
            raise RuntimeError(f"Admission response: {reply.code}")
        stop = monotonic() + 6
        while monotonic() < stop:
            reply = transport.query_report(
                request.coordinator, request.operation, allowance_us=500_000
            )
            if reply.code != 69:  # CoAP 2.05: retained report
                raise RuntimeError(f"Query response: {reply.code}")
            ack = o.apply_result(reply.payload, peer=request.coordinator)
            outcome = cbor2.loads(reply.payload)[5]
            if outcome:
                print(request.operation.value, ("Pending", "Succeeded",
                      "FailedClean", "Unresolved")[outcome])
                if outcome != 1:
                    raise RuntimeError("Operation did not succeed; retain its evidence")
                return ack
            sleep(0.02)
        raise TimeoutError("No final result; O retains the unresolved intent")

    artifact = Path("/tmp/kaos-guide-host/node.wasm").read_bytes()
    size, crc = transport.add_artifact(b"demo", artifact)
    addition = o.add_node(Node(
        logical_id=10, owner=1, artifact_ref=b"demo",
        artifact_bytes=size, artifact_crc32=crc, required_bytes=131072,
        configuration=b"\x00", addition_us=3_000_000, cleanup_us=1_000_000,
    ))
    complete(addition)
    removal = o.remove_node(addition.instance, cleanup_us=1_000_000, no_edges=True)
    ack = complete(removal)
    if ack is not None:
        transport.acknowledge(1, ack, allowance_us=500_000)
PY
```

Stop the host with Ctrl-C after the example. Use fresh isolated host/O sessions
for a new run; restarting O's allocator against existing device state is outside
the recovery contract. If a response is lost, query the original operation and
retain uncertainty. Do not resend construction merely because history is missing.

## Change a graph

| Python operation | Use |
|---|---|
| `o.add_node(node)` | Create an isolated instance |
| `o.add_node(node, channels=..., predicate_tokens=...)` | Create an instance and initial channels under one authorization |
| `o.add_edge(channel, cleanup_us=...)` | Connect established, usable nodes through compatible declared ports |
| `o.remove_edge(binding, cleanup_us=...)` | Remove the exact creation, preserving peer runtimes |
| `o.remove_node(instance, cleanup_us=...)` | Remove the instance and O's complete intended incidence |
| `o.remove_node(instance, cleanup_us=..., no_edges=True)` | Require complete empty incidence |

Each call creates an immutable authorized request. Submit it through
`GraphTransport.submit`, then incorporate queried evidence. The
[local and remote process examples](../verification/graph_core/tests/p5_process_test.py)
show `Port`, `Endpoint`, `Channel` and predicate construction. All nodes permit
dynamic binding, subject to lifecycle, declared-port and capacity checks.

## Write a guest

Export `int kaos_configure(const unsigned char *, unsigned)` returning zero on
success, and `void kaos_main(void)`. Main calls imported `kaos.init()` first,
then performs long-lived service work. There is no automatic restart.

| Import from `kaos` | Contract |
|---|---|
| `init()` | Queue the startup marker; control must still accept it |
| `error()` | Bounded reporting-only diagnostic; no automatic cleanup |
| `put(channel, data, size)` | Copy one message to outgoing storage; 0 means accepted |
| `get(channel, data, capacity)` | Copy and consume one incoming message; returns its byte count |
| `event_get(data, capacity)` | Consume one 32-byte channel-state notification; 0 means success |
| `event_wait(timeout_us)` | Cancellable wait, 0–1,000,000 µs; 0 means notification pending, 1 means timeout |

Data/event calls use errors -1 DISABLED, -2 FULL, -3 EMPTY, -4 INVALID and -5
SIZE as applicable. A channel argument points to three little-endian `u64`s
`(sourceLogical, destinationLogical, serviceId)` in guest memory. A notification
contains that 24-byte triple, little-endian `u32` permission bits (PUT=1, GET=2),
then four reserved zero bytes. Data calls require authorized main execution;
guest pointers never survive the call. Invalid or undersized reads leave the
message queued. The [sample guest](../verification/graph_core/guests/channel.c)
shows the import declarations and calls.

Artifacts must fit the restricted bytecode profile: 1–65536 bytes, one internal
memory with minimum and maximum one 64 KiB page, supported imports, no implicit
start/constructors and no WASI or guest threads. Configuration is at most 64
bytes. Use the [guest build flags](../verification/graph_core/CMakeLists.txt)
and qualify the actual artifact plus runtime arena; binary size is not its RAM
requirement.

## Store reports in the collector

Start the host with `KAOS_COLLECTOR=77:58840` to enroll a separate collector
socket. Given O's authorized `request` and a running device:

```python
from sim_env.graph_collector import GraphCollector

with GraphTransport(("127.0.0.1", 58840), {1: ("127.0.0.1", 58831)}) as db_transport:
    db = GraphCollector(db_transport, authority=99)
    db.register(request)
    db.collect(request.operation, allowance_us=500_000)
    db.consume(o, request.operation)
```

`collect` stores one observed version before ACKing the device; call it again
to collect later changes. `consume` validates archived evidence at O and releases
the exact acknowledged copies. A full archive raises `ArchiveFull` without
ACKing. This collector uses bounded RAM and explicit calls; it has no automatic
polling thread or persistence.

For direct handoff, `query_report` → `o.apply_result` → `acknowledge` follows
the same storage boundary. Convenience `transport.query(o, ...)` incorporates
evidence but does not send a storage ACK. Missing history proves neither object
absence nor a past outcome. Device retirement preserves live resources and
unresolved obligations; [architecture](architecture.md#evidence-and-time)
describes the retention policy.


## Build the networked ESP32 host

The ESP-IDF entrypoint runs the same `core_host_run()` as POSIX. It uses the
shared BSD-socket adapter, with ESP-IDF eventfd wakeups. Configure Wi-Fi under
Example Connection Configuration and enrolled identities/addresses under KaOS
graph host. The authority/artifact server must be reachable at the configured
IPv4 address and must enroll the board's address. Collector enrollment is
optional and separate from endpoint owners.

With ESP-IDF 5.5 loaded:

```sh
idf.py -C graph_core/ports/espidf -B /tmp/kaos-network-host menuconfig
idf.py -C graph_core/ports/espidf -B /tmp/kaos-network-host build
```

This target uses the four-node static profile, a 12 KiB control-task stack and
control priority 3 above worker priority 2. The default 28 KiB per-node reservation
includes the artifact and is provisional; match requests to the configured
profile. Successful compilation does not establish four-node Wi-Fi capacity or
application timing. The prior P7 local capacity fixture remains a separate target.
No host restart/rejoin recovery is introduced.
