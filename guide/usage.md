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
fixture needed by sibling Python regressions. The old P3/P4 executable names
remain optional build entrypoints for the same complete host, without separate
runtime behavior.

## Add and remove a node

In one terminal, start a fresh device host:

```sh
ASAN_OPTIONS=detect_stack_use_after_return=0 \
  /tmp/kaos-guide-host/core/kaos_host 58831 58830 1 99
```

Arguments are device UDP port, O's UDP/artifact port, device ID and O ID.
The host binds loopback and prints `LISTENING 58831`. Optional peers follow as
`device_id:udp_port` (loopback), or `device_id@IPv4:udp_port`; enroll each
participating device on its peers. The collector accepts the same address syntax.

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
[local and remote process examples](../../verification/graph_core/tests/p5_process_test.py)
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
message queued. The [sample guest](../../verification/graph_core/guests/channel.c)
shows the import declarations and calls.

Artifacts must fit the restricted bytecode profile: 1–65536 bytes, one internal
memory with minimum and maximum one 64 KiB page, supported imports, no implicit
start/constructors and no WASI or guest threads. Configuration is at most 64
bytes. Use the [guest build flags](../../verification/graph_core/CMakeLists.txt)
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
