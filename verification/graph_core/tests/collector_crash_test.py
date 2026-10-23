"""MGMT14/13: SIGKILL a RAM collector before/after exact storage ACK.

The observer receives boundary metadata, never the archived result payload.
It cannot supply the lost archive to O or convert a missing report to closure.
"""

import hashlib
import json
import multiprocessing
import os
import signal
import socket
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import patch

import cbor2
import p5_process_test as p5
from sim_env.graph_collector import GraphCollector
from sim_env.graph_evidence import AuthorizationError
from sim_env.graph_protocol import SCHEMA_VERSION
from sim_env.graph_transport import CHANGED, CONTENT, NOT_FOUND, GraphTransport


def archive_child(connection, address, peers, request):
    transport = GraphTransport(address, peers)
    try:
        collector = GraphCollector(transport, authority=99, capacity=1)
        collector.register(request)
        deadline = time.monotonic() + 5
        while True:
            response = transport.query_report(
                request.coordinator, request.operation, allowance_us=500_000
            )
            if response.code != CONTENT:
                raise AssertionError(f"collector query failed: {response.code}")
            report = cbor2.loads(response.payload)
            if report[5] == 1 and all(flags & 32 for flags in report[7]):
                break
            if time.monotonic() >= deadline:
                raise AssertionError("collector did not observe complete removal")
            time.sleep(0.005)
        ack = collector.store(response.payload, peer=request.coordinator)
        if collector.retrieve(request.operation) != (response.payload,):
            raise AssertionError("collector did not retain exact snapshot")
        connection.send(
            {
                "boundary": "stored_before_ack",
                "revision": report[4],
                "payload_sha256": hashlib.sha256(response.payload).hexdigest(),
            }
        )
        if connection.recv() != "acknowledge":
            raise AssertionError("unexpected collector command")
        acknowledgement = transport.acknowledge(
            request.coordinator, ack, allowance_us=500_000
        )
        if acknowledgement.code != CHANGED:
            raise AssertionError("device did not accept storage ACK")
        if collector.retrieve(request.operation) != (response.payload,):
            raise AssertionError("device ACK released archive without O's ACK")
        connection.send({"boundary": "device_acknowledged", "revision": report[4]})
        connection.recv()  # explicit hold until the parent kills this process
        raise AssertionError("collector unexpectedly released after ACK")
    finally:
        transport.close()
        connection.close()


class CollectorCrashTests(p5.NodeProcessTests):
    output = None

    def setUp(self):
        super().setUp()
        self.collector = self.connection = None

    def tearDown(self):
        try:
            if self.collector is not None:
                if self.collector.is_alive():
                    self.collector.kill()
                self.collector.join(timeout=5)
                self.assertFalse(self.collector.is_alive(), "collector did not stop")
                self.collector.close()
            if self.connection is not None:
                self.connection.close()
        finally:
            super().tearDown()

    def boundary(self, expected):
        self.assertTrue(
            self.connection.poll(10), f"missing collector boundary: {expected}"
        )
        event = self.connection.recv()
        self.assertEqual(event["boundary"], expected)
        return event

    def exercise_crash(self, after_ack):
        # Reserve the designated DB address before starting the real device.
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reservation:
            reservation.bind(("127.0.0.1", 0))
            address = reservation.getsockname()
            with patch.dict(os.environ, {"KAOS_COLLECTOR": f"77:{address[1]}"}):
                self.start(True)
            peer = self.generic_node(2, 20)
            channel = p5.Channel(
                p5.Endpoint(10, None, 1, 1),
                p5.Endpoint(20, peer.instance, 2, 2),
                7,
                3,
                4,
                2,
                2_000_000,
            )
            target = self.o.add_node(
                self.description(1, 10, False, generic=True),
                channels=(channel,),
                predicate_tokens=b"\x02\x00",
            )
            self.submit(target)
            self.succeeded(target)
            incidence = self.o.incidence(target.instance)
            self.assertEqual(len(incidence), 1)
            removal = self.o.remove_node(target.instance, cleanup_us=1_000_000)
            self.submit(removal)  # O receives admission only, never the final result
        peers = {
            int(process.args[4]): ("127.0.0.1", int(process.args[1]))
            for process in self.processes
        }
        context = multiprocessing.get_context("spawn")
        self.connection, child_connection = context.Pipe()
        self.collector = context.Process(
            target=archive_child, args=(child_connection, address, peers, removal)
        )
        self.collector.start()
        child_connection.close()
        stored = self.boundary("stored_before_ack")
        events = [stored]
        self.assertEqual(self.o.incidence(target.instance), incidence)
        if after_ack:
            self.connection.send("acknowledge")
            events.append(self.boundary("device_acknowledged"))
        self.collector.kill()
        self.collector.join(timeout=5)
        self.assertEqual(self.collector.exitcode, -signal.SIGKILL)
        events.append(
            {"boundary": "collector_killed", "exitcode": self.collector.exitcode}
        )
        post_crash_incidence = self.o.incidence(target.instance)
        self.assertEqual(post_crash_incidence, incidence)

        # A query through O's real transport applies only actual result evidence.
        # NOT_FOUND must leave the retained target available to incidence().
        if after_ack:
            response = self.transport.query(
                self.o, removal.operation, allowance_us=500_000
            )
            self.assertEqual(response.code, NOT_FOUND)
            self.assertEqual(
                cbor2.loads(response.payload),
                [SCHEMA_VERSION, 14, removal.operation.value, 1, []],
            )
            self.assertEqual(self.o.incidence(target.instance), incidence)
            disposition = "history_unavailable_authority_uncertain"
        else:
            response = self.transport.query_report(
                1, removal.operation, allowance_us=500_000
            )
            self.assertEqual(response.code, CONTENT)
            self.assertEqual(
                hashlib.sha256(response.payload).hexdigest(), stored["payload_sha256"]
            )
            self.assertEqual(self.o.incidence(target.instance), incidence)
            ack = self.o.apply_result(response.payload, peer=1)
            self.assertEqual(
                self.transport.acknowledge(1, ack, allowance_us=500_000).code, CHANGED
            )
            with self.assertRaises(AuthorizationError):
                self.o.incidence(target.instance)
            disposition = "device_copy_retrieved_authority_observed_closure"

        # Two new operations finish with the designated collector still dead.
        # Different logical identity: this is not recovery of the uncertain target.
        self.assert_live_success(self.query(peer))
        fresh = self.generic_node(1, 30)
        final = self.o.remove_node(fresh.instance, cleanup_us=1_000_000, no_edges=True)
        self.submit(final)
        closed = self.succeeded(final)
        self.assertEqual(closed[7][0] & 56, 56)
        if after_ack:
            self.assertEqual(self.o.incidence(target.instance), incidence)
        self.assertIsNone(self.processes[0].poll(), "collector crash killed the device")
        report = {
            "crash": "after_ack" if after_ack else "before_ack",
            "operation": removal.operation.value,
            "target": target.instance.value,
            "events": events,
            "disposition": disposition,
            "new_addition_and_removal_succeeded": True,
            "incidence_count_before_retrieval": len(post_crash_incidence),
            "authority_observed_closure_at_end": not after_ack,
            "scope": "two hosts; one channel; volatile collector loss; no recovery",
        }
        (self.output / f"{report['crash']}.json").write_text(
            json.dumps(report, indent=2) + "\n"
        )
        print(json.dumps(report), flush=True)

    def test_crash_before_storage_ack_preserves_device_copy(self):
        self.exercise_crash(False)

    def test_crash_after_storage_ack_preserves_authority_uncertainty(self):
        self.exercise_crash(True)


if __name__ == "__main__":
    CollectorCrashTests.executable = sys.argv[1]
    CollectorCrashTests.artifact = CollectorCrashTests.generic_artifact = Path(
        sys.argv[2]
    ).read_bytes()
    output = Path(sys.argv[3])
    output.mkdir(parents=True, exist_ok=True)
    CollectorCrashTests.output = Path(tempfile.mkdtemp(prefix="run-", dir=output))
    print(
        json.dumps({"collector_crash_output": str(CollectorCrashTests.output)}),
        flush=True,
    )
    suite = unittest.TestSuite(
        CollectorCrashTests(name)
        for name in vars(CollectorCrashTests)
        if name.startswith("test_")
    )
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(not result.wasSuccessful())
