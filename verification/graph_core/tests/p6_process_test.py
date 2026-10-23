"""P6 full removal through the existing authority, UDP port and WAMR runtime."""

import sys
import unittest
from pathlib import Path

import p5_process_test as p5
from sim_env.orchestrator import AuthorizationError

Channel, Endpoint = p5.Channel, p5.Endpoint


class RemovalProcessTests(p5.NodeProcessTests):
    def assert_removed(self, request):
        report = self.succeeded(request)
        self.assertTrue(all(facts & 56 == 56 for facts in report[7]), report)
        with self.assertRaisesRegex(AuthorizationError, "complete closure"):
            self.o.incidence(request.instance)
        return report

    def exercise_full_removal(self, remote):
        self.start(remote)
        peer = self.generic_node(2 if remote else 1, 20)
        channels = (
            Channel(Endpoint(10, None, 1, 1),
                    Endpoint(20, peer.instance, 2 if remote else 1, 2),
                    7, 3, 4, 2, 2_000_000),
            Channel(Endpoint(20, peer.instance, 2 if remote else 1, 1),
                    Endpoint(10, None, 1, 2),
                    8, 3, 4, 2, 2_000_000),
        )
        node = self.o.add_node(
            self.description(1, 10, False, generic=True), channels=channels,
            predicate_tokens=b"\x02\x00\x02\x01\x03\x00",
        )
        self.submit(node)
        self.succeeded(node)
        removal = self.o.remove_node(node.instance, cleanup_us=1_000_000)
        self.submit(removal)
        self.assertEqual(len(self.assert_removed(removal)[7]), 5)
        self.assert_live_success(self.query(peer))
        self.assertEqual(self.query(node)[5], 1)  # Historical addition success.

    def test_remote_incoming_and_outgoing(self):
        self.exercise_full_removal(True)

    def test_local_incoming_and_outgoing(self):
        self.exercise_full_removal(False)

    def test_authorized_edge_in_flight_is_fenced_at_other_coordinator(self):
        self.start(True)
        target = self.generic_node(1, 10)
        peer = self.generic_node(2, 20)
        channel = Channel(Endpoint(20, peer.instance, 2, 1),
                          Endpoint(10, target.instance, 1, 2),
                          7, 3, 4, 2, 2_000_000)
        edge = self.o.add_edge(channel, cleanup_us=1_000_000)
        delayed = self.o.dispatch(edge.operation)
        # Authorization already owns the binding, even before either device
        # receives its constructive command from the authority.
        removal = self.o.remove_node(target.instance, cleanup_us=1_000_000)
        self.assertEqual(removal.bindings, edge.bindings)
        self.submit(removal)
        self.assert_removed(removal)
        response = self.transport._exchange(
            delayed.coordinator, b"graph", lambda: delayed.payload, 1_000_000
        )
        self.assertEqual(response.code, 128, response)
        self.assert_live_success(self.query(peer))
        self.assertEqual(self.query(target)[5], 1)

    def test_missing_owner_retains_all_remote_obligations(self):
        self.start(True)
        peer = self.generic_node(2, 20)
        channel = Channel(Endpoint(20, peer.instance, 2, 1), Endpoint(10, None, 1, 2),
                          7, 3, 4, 2, 2_000_000)
        target = self.o.add_node(
            self.description(1, 10, False, generic=True), channels=(channel,),
            predicate_tokens=b"\x02\x00",
        )
        self.submit(target)
        self.succeeded(target)
        self.stop_remote()
        removal = self.o.remove_node(target.instance, cleanup_us=250_000)
        self.submit(removal)
        report = self.wait_report(removal, lambda report: report[5] == 3)
        self.assertEqual(report[7][0] & 56, 56, report)
        self.assertEqual(report[7][1] & 40, 8, report)
        self.assertEqual(report[7][2] & 56, 56, report)
        self.assertEqual(len(self.o.incidence(target.instance)), 1)
        self.assertEqual(self.query(target)[5], 1)


def load_tests(loader, tests, pattern):
    return unittest.TestSuite(
        RemovalProcessTests(name) for name in sorted(RemovalProcessTests.__dict__)
        if name.startswith("test_")
    )


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: p6_process_test.py HOST TRAFFIC_WASM")
    RemovalProcessTests.executable = str(Path(sys.argv[1]).resolve())
    traffic = Path(sys.argv[2])
    RemovalProcessTests.artifact = traffic.read_bytes()
    RemovalProcessTests.generic_artifact = traffic.with_name("node.wasm").read_bytes()
    unittest.main(argv=[sys.argv[0]], verbosity=2)
