"""P5 startup gates and optional ownership through real UDP and WAMR hosts."""

import sys
import time
import unittest
from pathlib import Path

import p4_process_test as p4

Channel, Endpoint, Node, Port = p4.Channel, p4.Endpoint, p4.Node, p4.Port


class NodeProcessTests(p4.EdgeProcessTests):
    generic_artifact = None

    def description(self, owner, logical, receiver, *, generic=False, allowance=2_000_000):
        if generic:
            size, crc = self.transport.add_artifact(b"node", self.generic_artifact)
            reference, configuration = b"node", b"\x00"
            ports = (Port(1, 1, 3, 4), Port(2, 0, 3, 4))
        else:
            size, crc = self.size, self.crc
            reference, configuration = b"traffic", bytes([receiver])
            ports = (Port(2 if receiver else 1, 0 if receiver else 1, 3, 4),)
        return Node(logical, owner, reference, size, crc, 131072,
                    configuration, allowance, 250_000, ports)

    def generic_node(self, owner, logical):
        request = self.o.add_node(self.description(owner, logical, False, generic=True))
        self.submit(request)
        self.succeeded(request)
        return request

    def wait_report(self, request, condition, timeout=4):
        deadline = time.monotonic() + timeout
        latest = None
        while time.monotonic() < deadline:
            latest = self.query(request)
            if condition(latest):
                return latest
            time.sleep(0.01)
        self.fail(f"missing expected channel progress: {latest}")

    def stop_remote(self):
        # O retains its accepted success and exact authorization. Losing Ki
        # supplies no remote closure evidence to the surviving coordinator.
        process = self.processes[1]
        process.terminate()
        self.assertEqual(process.wait(timeout=5), 0)

    def assert_live_success(self, report):
        self.assertEqual(report[5], 1, report)
        self.assertEqual(report[6] & 39, 39, report)  # commit, dispatch, STARTED, success
        self.assertEqual(report[6] & 24, 0, report)   # no whole-operation cleanup/cutoff
        self.assertEqual(report[7][0] & 252, 0, report)

    def required_channel(self, incoming):
        self.start(True)
        peer = self.add_node(2, 10 if incoming else 20, not incoming)
        new_logical = 20 if incoming else 10
        source = Endpoint(10, peer.instance if incoming else None,
                          2 if incoming else 1, 1)
        destination = Endpoint(20, None if incoming else peer.instance,
                               1 if incoming else 2, 2)
        channel = Channel(source, destination, 7, 3, 4, 2, 2_000_000)
        request = self.o.add_node(
            self.description(1, new_logical, incoming), channels=(channel,),
            predicate_tokens=b"\x02\x00",
        )
        self.submit(request)
        report = self.succeeded(request)
        self.assert_live_success(report)
        self.assertEqual(len(report[7]), 3)
        self.assertTrue(all(facts & 3 == 3 for facts in report[7][1:]), report)
        self.marker(request if incoming else peer, 1)
        self.assert_live_success(self.query(request))
        self.assert_live_success(self.query(peer))

    def test_required_incoming_channel_carries_payload(self):
        self.required_channel(True)

    def test_required_outgoing_channel_carries_payload(self):
        self.required_channel(False)

    def test_three_required_channels_share_remote_owner_record(self):
        self.start(True)
        source = self.generic_node(2, 10)
        channels = tuple(
            Channel(Endpoint(10, source.instance, 2, 1), Endpoint(20, None, 1, 2),
                    service, 3, 4, 2, 2_000_000)
            for service in (7, 8, 9)
        )
        request = self.o.add_node(
            self.description(1, 20, True, generic=True), channels=channels,
            predicate_tokens=b"\x02\x00\x02\x01\x03\x00\x02\x02\x03\x00",
        )
        self.submit(request)
        report = self.succeeded(request)
        self.assert_live_success(report)
        self.assertEqual(len(report[7]), 7)
        self.assertTrue(all(facts & 3 == 3 for facts in report[7][1:]), report)
        self.assert_live_success(self.query(source))

    def test_or_success_retains_missing_optional_owner_after_expiry(self):
        self.start(True)
        local = self.generic_node(1, 10)
        remote = self.generic_node(2, 20)
        self.stop_remote()
        channels = (
            Channel(Endpoint(20, remote.instance, 2, 1), Endpoint(30, None, 1, 2),
                    8, 3, 4, 2, 1_500_000),
            Channel(Endpoint(10, local.instance, 1, 1), Endpoint(30, None, 1, 2),
                    7, 3, 4, 2, 1_500_000),
        )
        request = self.o.add_node(
            self.description(1, 30, True, generic=True, allowance=1_000_000),
            channels=channels, predicate_tokens=b"\x02\x00\x02\x01\x04\x00",
        )
        self.submit(request)
        first = self.succeeded(request)
        self.assert_live_success(first)
        self.assertTrue(all(facts & 3 == 3 for facts in first[7][3:5]), first)
        self.assertEqual(first[7][1] & 2, 0, first)
        final = self.wait_report(
            request, lambda report: report[7][1] & 128 and report[7][2] & 32
        )
        self.assert_live_success(final)
        self.assertGreater(final[4], first[4])
        self.assertEqual(final[7][1] & 40, 8, final)  # selected remote cleanup, still unconfirmed
        self.assertEqual(final[7][2] & 56, 56, final)
        self.assertTrue(all(facts & 3 == 3 for facts in final[7][3:5]), final)
        self.assert_live_success(self.query(local))
        self.assertEqual(len(self.o.incidence(request.instance)), 2)

    def test_true_succeeds_with_all_optional_channels_unavailable(self):
        self.start(True)
        remote = self.generic_node(2, 10)
        self.stop_remote()
        channels = tuple(
            Channel(Endpoint(10, remote.instance, 2, 1), Endpoint(20, None, 1, 2),
                    service, 3, 4, 2, 1_500_000)
            for service in (7, 8)
        )
        request = self.o.add_node(
            self.description(1, 20, True, generic=True, allowance=1_000_000),
            channels=channels, predicate_tokens=b"\x01\x00",
        )
        self.submit(request)
        first = self.succeeded(request)
        self.assert_live_success(first)
        self.assertTrue(all(facts & 2 == 0 for facts in first[7][1:]), first)
        final = self.wait_report(
            request,
            lambda report: all(report[7][index] & 128 for index in (1, 3))
            and all(report[7][index] & 32 for index in (2, 4)),
        )
        self.assert_live_success(final)
        self.assertGreater(final[4], first[4])
        for index in (1, 3):
            self.assertEqual(final[7][index] & 40, 8, final)
        for index in (2, 4):
            self.assertEqual(final[7][index] & 56, 56, final)
        self.assertEqual(len(self.o.incidence(request.instance)), 2)


def load_tests(loader, tests, pattern):
    # Reuse the existing fixture helpers without repeating its P4 test cases.
    return unittest.TestSuite(
        NodeProcessTests(name) for name in sorted(NodeProcessTests.__dict__)
        if name.startswith("test_")
    )


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: p5_process_test.py HOST TRAFFIC_WASM")
    NodeProcessTests.executable = str(Path(sys.argv[1]).resolve())
    traffic = Path(sys.argv[2])
    NodeProcessTests.artifact = traffic.read_bytes()
    NodeProcessTests.generic_artifact = traffic.with_name("node.wasm").read_bytes()
    unittest.main(argv=[sys.argv[0]], verbosity=2)
