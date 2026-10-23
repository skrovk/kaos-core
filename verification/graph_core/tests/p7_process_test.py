"""P7 archive handoff, outages and retention through actual UDP/WAMR hosts."""

import importlib
import os
import socket
import sys
import time
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(
    0,
    os.environ.get(
        "KAOS_SIM_ENV", str(Path(__file__).resolve().parents[4] / "sim_env")
    ),
)

import cbor2
from sim_env.graph_collector import ArchiveFull, GraphCollector
from sim_env.graph_evidence import storage_ack
from sim_env.graph_protocol import SCHEMA_VERSION, OperationId
from sim_env.graph_transport import GraphTransport

p5 = importlib.import_module("p5_process_test")


class ReportingProcessTests(p5.NodeProcessTests):
    def setUp(self):
        super().setUp()
        self.db_transport = None

    def tearDown(self):
        if self.db_transport is not None:
            self.db_transport.close()
        super().tearDown()

    def start_archive(self, *, capacity=32, remote=False):
        reservation = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        reservation.bind(("127.0.0.1", 0))
        address = reservation.getsockname()
        try:
            with patch.dict(os.environ, {"KAOS_COLLECTOR": f"77:{address[1]}"}):
                self.start(remote)
        finally:
            reservation.close()
        peers = {int(p.args[4]): ("127.0.0.1", int(p.args[1])) for p in self.processes}
        self.db_transport = GraphTransport(address, peers)
        self.db = GraphCollector(self.db_transport, authority=99, capacity=capacity)

    def collect_terminal(self, request):
        deadline = time.monotonic() + 4
        while time.monotonic() < deadline:
            payload = self.db.collect(request.operation, allowance_us=500_000)
            if payload is not None and cbor2.loads(payload)[5] in (1, 2, 3):
                return cbor2.loads(payload)
            time.sleep(0.01)
        self.fail("collector did not observe terminal evidence")

    def assert_missing(self, request, current):
        response = self.db_transport.query_report(
            request.coordinator, request.operation, allowance_us=500_000
        )
        self.assertEqual(response.code, 132)
        body = cbor2.loads(response.payload)
        self.assertEqual(
            body[:4], [SCHEMA_VERSION, 14, request.operation.value, request.coordinator]
        )
        self.assertEqual(len(body[4]), 3 if current else 0)

    def test_archive_ack_keeps_live_runtime_and_accepts_later_evidence(self):
        self.start_archive()
        node = self.generic_node(1, 10)
        self.db.register(node)
        first = self.collect_terminal(node)
        self.assert_missing(node, True)
        removal = self.o.remove_node(node.instance, cleanup_us=1_000_000, no_edges=True)
        self.submit(removal)
        self.succeeded(removal)
        # The stored startup snapshot cannot retire the later exit/closure facts.
        self.db_transport.acknowledge(
            1, storage_ack(node.operation, 1, first[4]), allowance_us=500_000
        )
        latest = self.collect_terminal(node)
        self.assertGreater(latest[4], first[4])
        self.assertEqual(latest[5], 1)
        self.assertEqual(latest[7][0] & 56, 56)
        self.assertGreaterEqual(len(self.db.retrieve(node.operation)), 2)
        self.db.consume(self.o, node.operation)
        self.assertEqual(self.db.retrieve(node.operation), ())
        self.assert_missing(node, False)

    def test_collector_handoff_while_o_socket_is_unavailable(self):
        self.start_archive(remote=True)
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
        node = self.o.add_node(
            self.description(1, 10, False, generic=True),
            channels=(channel,),
            predicate_tokens=b"\x02\x00",
        )
        self.submit(node)
        self.succeeded(node)
        removal = self.o.remove_node(node.instance, cleanup_us=1_000_000)
        self.db.register(removal)
        self.submit(removal)
        self.transport.close()
        self.transport = None
        self.assertEqual(len(self.o.incidence(node.instance)), 1)
        final = self.collect_terminal(removal)
        self.assertEqual(final[5], 1)
        self.assertTrue(all(flags & 32 for flags in final[7]))
        self.assert_missing(removal, False)
        # O has retained authority in RAM but received no closure over its socket.
        self.assertEqual(len(self.o.incidence(node.instance)), 1)
        self.assertGreaterEqual(self.db.consume(self.o, removal.operation), 1)
        self.assertEqual(self.db.retrieve(removal.operation), ())
        with self.assertRaises(ValueError):
            self.o.incidence(node.instance)

    def test_db_full_never_acknowledges_or_blocks_removal(self):
        self.start_archive(capacity=1)
        node = self.generic_node(1, 10)
        self.db.register(node)
        self.collect_terminal(node)
        removal = self.o.remove_node(node.instance, cleanup_us=1_000_000, no_edges=True)
        self.db.register(removal)
        self.submit(removal)
        self.succeeded(removal)
        with self.assertRaises(ArchiveFull):
            self.db.collect(removal.operation, allowance_us=500_000)
        response = self.db_transport.query_report(
            1, removal.operation, allowance_us=500_000
        )
        self.assertEqual(response.code, 69)
        self.assertEqual(cbor2.loads(response.payload)[5], 1)
        self.db.consume(self.o, node.operation)
        self.assertEqual(self.collect_terminal(removal)[5], 1)
        self.assert_missing(removal, False)

    def test_db_loss_does_not_block_runtime_and_direct_handoff(self):
        self.start_archive()
        self.db_transport.close()
        self.db_transport = None
        first = None
        for cycle in range(2):
            node = self.generic_node(1, 10 + cycle)
            removal = self.o.remove_node(
                node.instance, cleanup_us=1_000_000, no_edges=True
            )
            self.submit(removal)
            self.succeeded(removal)
            first = first or removal
        # Four original submissions fill the ordinary CoAP response cache.
        # Both lifecycles completed without DB availability or storage ACKs.
        # Deterministic C tests exercise record pressure independently of the
        # transport cache's much longer response-retention lifetime.
        response = self.transport.query_report(1, first.operation, allowance_us=500_000)
        self.assertEqual(response.code, 69)
        exact_ack = self.o.apply_result(response.payload, peer=1)
        self.transport.acknowledge(1, exact_ack, allowance_us=500_000)
        response = self.transport.query_report(1, first.operation, allowance_us=500_000)
        self.assertEqual(response.code, 132)
        self.assertEqual(cbor2.loads(response.payload)[4], [])
        unknown = self.transport.query_report(
            1, OperationId(99999), allowance_us=500_000
        )
        self.assertEqual(unknown.code, 132)
        self.assertEqual(cbor2.loads(unknown.payload)[4], [])
        response = self.transport.acknowledge(
            1, storage_ack(first.operation, 1, 1), allowance_us=500_000
        )
        self.assertEqual(response.code, 68)


if __name__ == "__main__":
    ReportingProcessTests.executable = sys.argv[1]
    ReportingProcessTests.artifact = Path(sys.argv[2]).read_bytes()
    ReportingProcessTests.generic_artifact = (
        Path(sys.argv[2]).with_name("node.wasm").read_bytes()
    )
    suite = unittest.TestSuite(
        ReportingProcessTests(name)
        for name in vars(ReportingProcessTests)
        if name.startswith("test_")
    )
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(not result.wasSuccessful())
