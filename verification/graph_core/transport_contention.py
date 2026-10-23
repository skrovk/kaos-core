"""Repeat one POSIX feedback/artifact contention experiment.

Select the old or new sim_env package using PYTHONPATH, and pair it with its
matching host executable. Both runs use the same guests, phases and evaluator.
The JSON describes observed process timing; it declares no latency target.
"""

import argparse
import hashlib
import json
import math
import struct
import time
from pathlib import Path

from sim_env.graph_protocol import Channel, Endpoint, Node, Port
from sim_env.graph_run import GraphRun
from sim_env.graph_transport import CONTENT, CREATED, GET, decode
from sim_env.graph_workloads import evaluate, read_trace


def uleb(value):
    encoded = bytearray()
    while value >= 128:
        encoded.append((value & 127) | 128)
        value >>= 7
    encoded.append(value)
    return bytes(encoded)


def padded_wasm(data, target=16384):
    """Append one valid custom section, preserving all executable sections."""
    if data[:8] != b"\x00asm\x01\x00\x00\x00" or len(data) >= target - 32:
        raise ValueError("node guest must be a small version-one Wasm module")
    name = b"kaos.transport.padding"
    prefix = uleb(len(name)) + name
    for length in range(1, 6):
        body_size = target - len(data) - 1 - length
        if len(uleb(body_size)) == length:
            break
    else:
        raise ValueError("target size cannot be reached with one custom section")
    return data + b"\x00" + uleb(body_size) + prefix + bytes(body_size - len(prefix))


def distribution(values):
    values = sorted(values)
    return {
        "count": len(values),
        "p50_us": values[math.ceil(len(values) * 0.50) - 1] if values else None,
        "p95_us": values[math.ceil(len(values) * 0.95) - 1] if values else None,
        "max_us": values[-1] if values else None,
    }


def analyze(events, phases, initiator, responder, reference):
    samples = evaluate(events, (1, 2), "feedback", phases)
    records, complete = read_trace(events, (1, 2))
    if not complete:
        raise ValueError("incomplete channel trace; timing evidence is inconclusive")
    boundaries = {}
    for record in records:
        sequence = struct.unpack_from("<I", record["payload"])[0]
        key = record["instance"], record["direction"], record["channel"], sequence
        boundaries[key] = min(
            boundaries.get(key, record["timestamp_us"]), record["timestamp_us"]
        )
    results = []
    for phase, sample in zip(phases, samples["phases"]):
        first_get = {}
        for record in records:
            if (
                record["instance"] != initiator
                or record["direction"] != "GET"
                or record["channel"] != (20, 10, 1)
                or not phase["begin_us"] <= record["timestamp_us"] < phase["end_us"]
            ):
                continue
            sequence = struct.unpack_from("<I", record["payload"])[0]
            first_get[sequence] = min(
                first_get.get(sequence, record["timestamp_us"]), record["timestamp_us"]
            )
        elapsed = []
        for sequence, consumed in first_get.items():
            origin = boundaries.get((initiator, "PUT", (10, 20, 1), sequence))
            received = boundaries.get((responder, "GET", (10, 20, 1), sequence))
            published = boundaries.get((responder, "PUT", (20, 10, 1), sequence))
            if (
                None in (origin, received, published)
                or not origin <= received <= published <= consumed
            ):
                raise ValueError(
                    "feedback GET lacks an ordered source/responder copy path"
                )
            elapsed.append(consumed - origin)
        results.append(
            {
                **sample,
                "duration_us": phase["end_us"] - phase["begin_us"],
                "source_put_to_final_get": distribution(elapsed),
            }
        )

    transport = {
        str(device): {"available": False, "reason": "no TRANSPORT stdout summary"}
        for device in (1, 2)
    }
    for event in events:
        if event["kind"] == "host_stdout" and event["line"].startswith("TRANSPORT "):
            fields = dict(part.split("=", 1) for part in event["line"].split()[1:])
            transport[str(event["device"])] = {
                "available": True,
                **{key: int(value) for key, value in fields.items()},
            }

    # Correlate the padded guest's CoAP requests with their block responses.
    # Both relay versions record unchanged wire payloads and delivery times.
    requests, responses = [], []
    for event in events:
        if event["kind"] != "packet_delivered" or {
            event["source"],
            event["destination"],
        } != {1, 99}:
            continue
        message = decode(bytes.fromhex(event["payload_hex"]))
        path = tuple(value for number, value in message.options if number == 11)
        if (
            event["source"] == 1
            and message.code == GET
            and path == (b"artifact", reference.hex().encode())
        ):
            requests.append((message.token, event["delivered_us"]))
        elif (
            event["source"] == 99
            and message.code == CONTENT
            and message.option(23) is not None
        ):
            responses.append((message.token, event["delivered_us"], message.option(23)))
    tokens = {token for token, _ in requests}
    delivered = [
        (timestamp, int.from_bytes(block, "big"))
        for token, timestamp, block in responses
        if token in tokens
    ]
    final = [timestamp for timestamp, block in delivered if not block & 8]
    if not requests or not final:
        raise ValueError("padded artifact lacks an observed complete block transfer")
    begin, end = min(timestamp for _, timestamp in requests), max(final)
    transfer_gets = [
        record
        for record in records
        if record["instance"] == initiator
        and record["direction"] == "GET"
        and record["channel"] == (20, 10, 1)
        and begin <= record["timestamp_us"] <= end
    ]
    transfer = {
        "begin_us": begin,
        "end_us": end,
        "duration_us": end - begin,
        "requests": len(requests),
        "responses": len(delivered),
        "feedback_gets_during_block_transfer": len(transfer_gets),
    }
    passed = all(
        phase["unique_sequences"] >= (1 if phase["name"] == "artifact" else 8)
        for phase in results
    )
    return {
        "status": "passed" if passed else "failed",
        "trace_complete": True,
        "observations": len(records),
        "phases": results,
        "artifact_block_transfer": transfer,
        "transport": transport,
    }


def run_experiment(host, feedback_guest, node_guest, output):
    phases, outcomes = [], []
    reference = b"padded-node"
    padded = padded_wasm(Path(node_guest).read_bytes())
    with GraphRun(host, output, (1, 2), seed=0, trace_channels=True) as run:
        (Path(output) / "padded-node.wasm").write_bytes(padded)
        size, crc = run.add_artifact(b"feedback", Path(feedback_guest).read_bytes())
        padded_size, padded_crc = run.add_artifact(reference, padded)

        def complete(request):
            if run.submit(request).code != CREATED:
                raise RuntimeError("contention operation was not admitted")
            report = run.wait(request)
            outcomes.append(
                {
                    "operation": request.operation.value,
                    "kind": int(request.kind),
                    "report": report,
                }
            )
            if report[5] != 1:
                raise RuntimeError(f"contention operation did not succeed: {report}")
            return request

        def node(logical, owner, role):
            return Node(
                logical,
                owner,
                b"feedback",
                size,
                crc,
                131072,
                bytes([role]),
                5_000_000,
                3_000_000,
                (Port(1, 0, 1, 64), Port(2, 1, 1, 64)),
            )

        def channel(source, destination):
            return Channel(source, destination, 1, 1, 64, 2, 5_000_000)

        responder = complete(run.o.add_node(node(20, 2, 1)))
        initiator = complete(
            run.o.add_node(
                node(10, 1, 0),
                channels=(
                    channel(
                        Endpoint(10, None, 1, 2), Endpoint(20, responder.instance, 2, 1)
                    ),
                    channel(
                        Endpoint(20, responder.instance, 2, 2), Endpoint(10, None, 1, 1)
                    ),
                ),
                predicate_tokens=b"\x02\x00\x02\x01\x03\x00",
            )
        )
        instances = {"10": initiator.instance.value, "20": responder.instance.value}

        def phase(name, action):
            begin = time.monotonic_ns() // 1000
            run.emit("contention_phase_begin", name=name)
            result = action()
            end = time.monotonic_ns() // 1000
            run.emit("contention_phase_end", name=name)
            phases.append(
                {"name": name, "begin_us": begin, "end_us": end, "instances": instances}
            )
            return result

        phase("before", lambda: time.sleep(0.25))
        # The second slot on the initiator's host shares the control task and
        # registry lock with ongoing feedback. wait() queries progress every
        # 10ms, adding management service during the artifact transfer.
        added = phase(
            "artifact",
            lambda: complete(
                run.o.add_node(
                    Node(
                        30,
                        1,
                        reference,
                        padded_size,
                        padded_crc,
                        131072,
                        b"\x00",
                        5_000_000,
                        3_000_000,
                        (),
                    )
                )
            ),
        )
        phase("after", lambda: time.sleep(0.25))
        complete(run.o.remove_node(added.instance, cleanup_us=3_000_000, no_edges=True))
        complete(run.o.remove_node(initiator.instance, cleanup_us=3_000_000))
        complete(
            run.o.remove_node(responder.instance, cleanup_us=3_000_000, no_edges=True)
        )

    events = [
        json.loads(line)
        for line in (Path(output) / "events.jsonl").read_text().splitlines()
    ]
    result = analyze(
        events, phases, initiator.instance.value, responder.instance.value, reference
    )
    result.update(
        {
            "workload": "two-device 64-byte feedback plus 16KiB artifact on initiator host",
            "script_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            "padded_artifact": {
                "bytes": len(padded),
                "sha256": hashlib.sha256(padded).hexdigest(),
            },
            "outcomes": outcomes,
            "windows": phases,
            "remaining_instances_at_teardown": [],
            "clock": "common POSIX host CLOCK_MONOTONIC microseconds",
            "latency_boundary": "first successful source PUT to first final feedback GET per sequence",
            "latency_cohort": "first final GET for each sequence within each phase; origin may precede phase",
            "limitations": [
                "One traced process run; host and relay scheduling are not deterministic",
                "Observed GET latency excludes missing samples and is not a delivery guarantee",
                "GET is a copy to guest memory, not guest acceptance or physical execution",
                "No acquisition time, cross-machine synchronization, ESP32 timing or workload acceptance target",
                "Instrumentation overhead and lower-layer socket loss are not isolated",
                "Artifact operation window also includes runtime preparation and report observation",
            ],
        }
    )
    with (Path(output) / "result.json").open("x") as stream:
        stream.write(json.dumps(result, indent=2) + "\n")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, type=Path)
    parser.add_argument("--feedback-guest", required=True, type=Path)
    parser.add_argument("--node-guest", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        result = run_experiment(**vars(args))
    except (OSError, RuntimeError, ValueError, TimeoutError) as error:
        parser.exit(2, f"contention experiment failed: {error}\n")
    print(json.dumps(result, indent=2))
    if result["status"] != "passed":
        parser.exit(1)


if __name__ == "__main__":
    main()
