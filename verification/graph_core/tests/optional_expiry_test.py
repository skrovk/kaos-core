"""APP03 optional expiry after success, followed by positively observed cleanup."""

import argparse
import json
import tempfile
import time
from pathlib import Path

from sim_env.graph_alternatives import evaluate_alternatives
from sim_env.graph_protocol import Channel, Endpoint, Node, Port
from sim_env.graph_run import GraphRun
from sim_env.graph_transport import CREATED


def run_expiry(host, guest, output, first):
    selected = 0 if first == "A" else 1
    cold_owner = 3 - selected
    phases, outcomes = [], []
    with GraphRun(host, output, (1, 2, 3), seed=42, trace_channels=True) as run:
        size, crc = run.add_artifact(b"alternatives", guest.read_bytes())

        def complete(request):
            if run.submit(request).code != CREATED:
                raise RuntimeError("expiry fixture request not admitted")
            report = run.wait(request)
            if report[5] != 1:
                raise RuntimeError(f"expiry fixture operation failed: {report}")
            outcomes.append(report)
            return request

        def node(logical, owner, role):
            return Node(
                logical,
                owner,
                b"alternatives",
                size,
                crc,
                131072,
                bytes([role]),
                1_000_000,
                4_000_000,
                (Port(1, 0, 1, 64), Port(2, 1, 1, 64)),
            )

        def phase(name):
            begin = time.monotonic_ns() // 1000
            run.emit("workload_phase_begin", name=name)
            time.sleep(0.25)
            phases.append(
                {
                    "name": name,
                    "begin_us": begin,
                    "end_us": time.monotonic_ns() // 1000,
                    "active": [first],
                }
            )
            run.emit("workload_phase_end", name=name)

        sink_a = complete(run.o.add_node(node(20, 2, 1)))
        sink_b = complete(run.o.add_node(node(30, 3, 2)))
        run.network.set_link(1, cold_owner, loss=1)
        channels = tuple(
            Channel(
                Endpoint(10, None, 1, 2),
                Endpoint(logical, peer.instance, owner, 1),
                1,
                1,
                64,
                2,
                1_000_000 if owner == cold_owner else 5_000_000,
            )
            for logical, owner, peer in ((20, 2, sink_a), (30, 3, sink_b))
        )
        source = complete(
            run.o.add_node(
                node(10, 1, 0),
                channels=channels,
                predicate_tokens=b"\x02\x00\x02\x01\x04\x00",
            )
        )
        startup = outcomes[-1]

        def live(report):
            if (
                report[5] != 1
                or report[6] != 39
                or report[7][0] != 1
                or report[7][1 + 2 * selected : 3 + 2 * selected] != [3, 3]
                or any(f & 2 for f in report[7][3 - 2 * selected : 5 - 2 * selected])
            ):
                raise RuntimeError(f"live supported source not established: {report}")

        def observe(closed):
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                try:
                    report = run.query(source, allowance_us=250_000)
                except TimeoutError:
                    report = None
                if report is not None:
                    live(report)
                    other = report[7][3 - 2 * selected : 5 - 2 * selected]
                    expired = any(f & 128 for f in other)
                    cleaned = all(f & 56 == 56 for f in other)
                    if expired and (cleaned if closed else not cleaned):
                        return report
                time.sleep(0.01)
            raise TimeoutError("optional expiry/cleanup evidence not observed")

        live(startup)
        if any(
            f not in (0, 1) for f in startup[7][3 - 2 * selected : 5 - 2 * selected]
        ):
            raise RuntimeError(
                "optional branch already expired before observed startup"
            )
        phase("startup_supported")
        expired = observe(False)
        run.emit("optional_expired", report=expired)
        phase("expired_unresolved")
        run.network.set_link(1, cold_owner)
        cleaned = observe(True)
        run.emit("optional_cleaned", report=cleaned)
        phase("expired_cleaned")
        complete(run.o.remove_node(source.instance, cleanup_us=4_000_000))
        for peer in (sink_a, sink_b):
            complete(
                run.o.remove_node(peer.instance, cleanup_us=4_000_000, no_edges=True)
            )
        instances = {
            "10": source.instance.value,
            "20": sink_a.instance.value,
            "30": sink_b.instance.value,
        }

    events = [
        json.loads(row) for row in (output / "events.jsonl").read_text().splitlines()
    ]
    result = evaluate_alternatives(events, (1, 2, 3), instances, phases)
    result.update(
        first=first,
        startup_report=startup,
        expired_report=expired,
        cleaned_report=cleaned,
        windows=phases,
        instances=instances,
        outcomes=outcomes,
        remaining_instances_at_teardown=[],
        scope="APP03 optional expiry and cleanup after success; existing branch keeps carrying samples",
    )
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    if result["status"] != "passed":
        raise AssertionError(result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", type=Path)
    parser.add_argument("guest", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    def both(root):
        root.mkdir(exist_ok=False)
        for first in ("A", "B"):
            result = run_expiry(args.host, args.guest, root / first, first)
            print(first, result["status"], result["phases"])

    if args.output:
        both(args.output)
    else:
        with tempfile.TemporaryDirectory(prefix="kaos-expiry-") as temporary:
            both(Path(temporary) / "runs")


if __name__ == "__main__":
    main()
