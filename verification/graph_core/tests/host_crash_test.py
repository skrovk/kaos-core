"""HOST03: two active sinks share one crash-stop failure domain."""

import json
import signal
import sys
import tempfile
import time
import unittest
from pathlib import Path

from sim_env.graph_protocol import Channel, Endpoint, Node, Port
from sim_env.graph_run import GraphRun
from sim_env.graph_transport import CREATED


def events(directory):
    # Read only complete lines while stdout readers append observations.
    return [
        json.loads(line)
        for line in (directory / "events.jsonl").read_text().split("\n")[:-1]
    ]


class HostCrashTests(unittest.TestCase):
    host = guest = output = None

    def test_shared_host_crash_preserves_remote_cleanup_obligations(self):
        directory = self.output / "shared"
        with GraphRun(self.host, directory, devices=(1, 2), trace_channels=True) as run:
            size, crc = run.add_artifact(b"alternatives", self.guest.read_bytes())

            def node(logical, owner, role):
                return Node(
                    logical,
                    owner,
                    b"alternatives",
                    size,
                    crc,
                    131072,
                    bytes([role]),
                    5_000_000,
                    1_000_000,
                    (Port(1, 0, 1, 64), Port(2, 1, 1, 64)),
                )

            def complete(request):
                self.assertEqual(run.submit(request).code, CREATED)
                report = run.wait(request)
                self.assertEqual(report[5], 1, report)
                return request

            sink_a = complete(run.o.add_node(node(20, 2, 1)))
            sink_b = complete(run.o.add_node(node(30, 2, 2)))
            channels = tuple(
                Channel(
                    Endpoint(10, None, 1, 2),
                    Endpoint(logical, sink.instance, 2, 1),
                    1,
                    1,
                    64,
                    2,
                    5_000_000,
                )
                for logical, sink in ((20, sink_a), (30, sink_b))
            )
            source = complete(
                run.o.add_node(
                    node(10, 1, 0),
                    channels=channels,
                    predicate_tokens=b"\x02\x00\x02\x01\x03\x00",
                )
            )
            incidence = run.o.incidence(source.instance)
            self.assertEqual(len(incidence), 2)
            expected = {sink_a.instance.value, sink_b.instance.value}
            deadline = time.monotonic() + 5
            while True:
                observed = set()
                for event in events(directory):
                    if event["kind"] == "host_stdout" and event["device"] == 2:
                        fields = event["line"].split()
                        if (
                            len(fields) == 9
                            and fields[0] == "CHANNEL"
                            and fields[4] == "GET"
                        ):
                            observed.add(int(fields[3]))
                if observed == expected:
                    break
                self.assertLess(
                    time.monotonic(), deadline, "both sinks must receive before crash"
                )
                time.sleep(0.005)
            run.emit(
                "crash_precondition", device=2, receiving_instances=sorted(observed)
            )
            with self.assertRaises(ValueError):
                run.crash(99)  # authority is not a managed device
            run.crash(2)
            with self.assertRaises(RuntimeError):
                run.crash(2)  # crash-stop, no implicit restart or idempotent repair
            self.assertEqual(run.o.incidence(source.instance), incidence)
            for sink in (sink_a, sink_b):
                with self.assertRaises(TimeoutError):
                    run.query(sink, allowance_us=50_000)
            removal = run.o.remove_node(source.instance, cleanup_us=300_000)
            self.assertEqual(run.submit(removal).code, CREATED)
            unresolved = run.wait(removal)
            self.assertEqual(unresolved[5], 3, unresolved)
            self.assertEqual(unresolved[6] & 24, 24)
            self.assertEqual(len(unresolved[7]), 5)
            for index in (0, 1, 3):
                self.assertEqual(unresolved[7][index] & 56, 56)
            for index in (2, 4):
                self.assertFalse(unresolved[7][index] & 32)
            self.assertEqual(run.o.incidence(source.instance), incidence)

        captured = events(directory)
        crashes = [e for e in captured if e["kind"] == "device_crashed"]
        self.assertEqual(len(crashes), 1)
        self.assertEqual(
            (crashes[0]["device"], crashes[0]["returncode"]), (2, -signal.SIGKILL)
        )
        exit_record = next(
            e for e in captured if e["kind"] == "process_exit" and e["device"] == 2
        )
        self.assertEqual(exit_record["returncode"], -signal.SIGKILL)
        self.assertEqual(captured[-1]["errors"], [])
        self.assertFalse(captured[-1]["proves_graph_cleanup"])
        self.assertTrue(
            any(e["kind"] == "report" and e["report"] == unresolved for e in captured)
        )
        self.assertFalse(
            any(
                e["kind"] == "host_stdout"
                and e["device"] == 2
                and e["line"].startswith("CHANNEL_END ")
                for e in captured
            )
        )
        self.assertEqual(run.o.incidence(source.instance), incidence)
        (directory / "assessment.json").write_text(
            json.dumps(
                {
                    "status": "passed",
                    "crashed_device": 2,
                    "active_instances_before_crash": sorted(observed),
                    "removal_report": unresolved,
                    "retained_incidence": len(incidence),
                    "scope": "positive shared-host crash and unresolved cleanup, not complete trace or recovery",
                },
                indent=2,
            )
            + "\n"
        )

    def test_unrequested_kill_still_fails_run(self):
        directory = self.output / "unexpected"
        with (
            self.assertRaisesRegex(RuntimeError, "host 1 exit"),
            GraphRun(self.host, directory, devices=(1,)) as run,
        ):
            # Test-only fault bypass: production crash API must be used
            # for an intentional fault, not every SIGKILL waived globally.
            process = run._processes[1]
            process.kill()
            self.assertEqual(process.wait(timeout=5), -signal.SIGKILL)
            with self.assertRaisesRegex(RuntimeError, "exited during"):
                run._check()
        self.assertTrue(events(directory)[-1]["errors"])


if __name__ == "__main__":
    HostCrashTests.host, HostCrashTests.guest = map(Path, sys.argv[1:3])
    output = Path(sys.argv[3])
    output.mkdir(parents=True, exist_ok=True)
    HostCrashTests.output = Path(tempfile.mkdtemp(prefix="run-", dir=output))
    print(json.dumps({"host_crash_output": str(HostCrashTests.output)}), flush=True)
    result = unittest.TextTestRunner(verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(HostCrashTests)
    )
    raise SystemExit(not result.wasSuccessful())
