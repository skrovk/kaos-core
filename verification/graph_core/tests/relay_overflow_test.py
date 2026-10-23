"""OBS03: actual relay overflow rejects a run; requested loss does not."""

import json
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest.mock import patch

from sim_env.graph_protocol import OperationId
from sim_env.graph_relay import GraphRelay
from sim_env.graph_run import GraphRun
from sim_env.graph_transport import NOT_FOUND


class RelayOverflowTests(unittest.TestCase):
    host = output = None

    def exercise(self, overflow):
        changed = threading.Condition()
        events = []

        def relay_factory(participants, emit, **options):
            def observe(kind, **fields):
                emit(kind, **fields)
                with changed:
                    events.append({"kind": kind, **fields})
                    changed.notify_all()

            # Use the actual UDP relay with its existing capacity parameter.
            return GraphRelay(participants, observe, queue_limit=1, **options)

        reason = "relay_overflow" if overflow else "loss"
        directory = self.output / reason

        def run_query():
            with GraphRun(self.host, directory, devices=(1,)) as run:
                # Positive round trip through both proxy directions.
                response = run.transport.query_report(
                    1, OperationId(42), allowance_us=500_000
                )
                self.assertEqual(response.code, NOT_FOUND)
                if overflow:
                    run.network.set_link(99, 1, delay_ms=10000, duplicate=1)
                else:
                    run.network.set_link(99, 1, loss=1)
                # Less than the retry interval: exactly one query, two copies
                # only in the duplication case. Its timeout is not our oracle.
                with self.assertRaises(TimeoutError):
                    run.transport.query_report(1, OperationId(43), allowance_us=50_000)
                with changed:
                    self.assertTrue(
                        changed.wait_for(
                            lambda: any(
                                e["kind"] == "packet_dropped" and e["reason"] == reason
                                for e in events
                            ),
                            timeout=2,
                        ),
                        f"missing actual {reason} event",
                    )
                run.network.check()
                # Exit a successful body: overflow must independently fail
                # GraphRun's teardown, without relying on the query timeout.

        with patch("sim_env.graph_run.GraphRelay", side_effect=relay_factory):
            if overflow:
                with self.assertRaisesRegex(RuntimeError, "relay queue overflow"):
                    run_query()
            else:
                run_query()

        captured = [
            json.loads(line)
            for line in (directory / "events.jsonl").read_text().splitlines()
        ]
        teardown = captured[-1]
        self.assertEqual(teardown["kind"], "teardown")
        self.assertIs(teardown["proves_graph_cleanup"], False)
        self.assertEqual(teardown["relay_overflow_drops"], int(overflow))
        drops = [
            e for e in events if e["kind"] == "packet_dropped" and e["reason"] == reason
        ]
        self.assertEqual(len(drops), 1)
        dropped = drops[0]
        self.assertEqual((dropped["source"], dropped["destination"]), (99, 1))
        decisions = [
            e
            for e in events
            if e["kind"] == "packet_decision" and e["packet_id"] == dropped["packet_id"]
        ]
        delivered = [
            e
            for e in events
            if e["kind"] == "packet_delivered"
            and e["packet_id"] == dropped["packet_id"]
        ]
        self.assertEqual(delivered, [])
        if overflow:
            self.assertEqual(
                [(e["copy"], e["reason"]) for e in decisions],
                [(0, None), (1, "relay_overflow")],
            )
            ended = [
                e
                for e in events
                if e["kind"] == "packet_dropped" and e["reason"] == "run_ended"
            ]
            self.assertEqual(len(ended), 1)
            self.assertEqual(
                (ended[0]["packet_id"], ended[0]["copy"]), (dropped["packet_id"], 0)
            )
            self.assertEqual(len(teardown["errors"]), 1)
            self.assertIn("relay queue overflow", teardown["errors"][0])
        else:
            self.assertEqual(len(decisions), 1)
            self.assertEqual(teardown["errors"], [])
        print(
            json.dumps(
                {
                    "case": reason,
                    "actual_drops": 1,
                    "run_rejected": overflow,
                    "teardown": teardown,
                }
            ),
            flush=True,
        )

    def test_actual_overflow_rejects_run(self):
        self.exercise(True)

    def test_requested_loss_does_not_reject_run(self):
        self.exercise(False)


if __name__ == "__main__":
    RelayOverflowTests.host = Path(sys.argv[1])
    output = Path(sys.argv[2])
    output.mkdir(parents=True, exist_ok=True)
    RelayOverflowTests.output = Path(tempfile.mkdtemp(prefix="run-", dir=output))
    print(
        json.dumps({"relay_overflow_output": str(RelayOverflowTests.output)}),
        flush=True,
    )
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(RelayOverflowTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(not result.wasSuccessful())
