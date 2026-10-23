"""P4 local/remote edge lifecycle with actual C hosts and WAMR guests."""

import os
import queue
import socket
import subprocess
import sys
import threading
import time
import unittest
from pathlib import Path

# Keep this slice in kaos_core while using the sibling authority implementation.
sys.path.insert(0, str(Path(__file__).resolve().parents[4] / "sim_env"))

import cbor2
from sim_env.graph_protocol import Channel, Endpoint, Node, Port
from sim_env.graph_transport import GraphTransport
from sim_env.orchestrator import Orchestrator


class EdgeProcessTests(unittest.TestCase):
    executable = None
    artifact = None

    def setUp(self):
        self.processes = []
        self.readers = []
        self.lines = queue.Queue()
        self.diagnostics = {}
        self.transport = None

    def tearDown(self):
        for process in self.processes:
            if process.poll() is None:
                process.terminate()
        failures = []
        for process in self.processes:
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
                failures.append("host failed to stop within five seconds")
            errors = process.stderr.read()
            process.stderr.close()
            if process.returncode != 0 or errors:
                failures.append(f"exit={process.returncode}: {errors}")
        for reader in self.readers:
            reader.join(timeout=1)
        for process in self.processes:
            process.stdout.close()
        if self.transport is not None:
            self.transport.close()
        self.assertEqual(failures, [], "\n".join(failures))

    def start(self, remote):
        owners = (1, 2) if remote else (1,)
        reservations = []
        addresses = {}
        application_addresses = {}
        try:
            for owner in owners:
                probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                probe.bind(("127.0.0.1", 0))
                reservations.append(probe)
                addresses[owner] = probe.getsockname()
                application_probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                application_probe.bind(("127.0.0.1", 0))
                reservations.append(application_probe)
                application_addresses[owner] = application_probe.getsockname()
            self.transport = GraphTransport(("127.0.0.1", 0), addresses)
        finally:
            for probe in reservations:
                probe.close()
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = (
            environment.get("ASAN_OPTIONS", "") + ":detect_stack_use_after_return=0"
        )
        for owner in owners:
            arguments = [
                self.executable,
                str(addresses[owner][1]),
                str(application_addresses[owner][1]),
                str(self.transport.address[1]),
                str(owner),
                "99",
            ]
            arguments.extend(
                f"{peer}:{address[1]}:{application_addresses[peer][1]}"
                for peer, address in addresses.items()
                if peer != owner
            )
            process = subprocess.Popen(
                arguments,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                env=environment,
            )
            self.processes.append(process)

            def read_lines(process=process, owner=owner):
                for line in process.stdout:
                    self.lines.put((owner, line.strip()))

            reader = threading.Thread(target=read_lines, daemon=True)
            reader.start()
            self.readers.append(reader)
            self.assertEqual(
                self.lines.get(timeout=5),
                (
                    owner,
                    f"LISTENING {addresses[owner][1]} {application_addresses[owner][1]}",
                ),
            )
        self.o = Orchestrator(owners)
        self.size, self.crc = self.transport.add_artifact(b"traffic", self.artifact)

    def submit(self, request):
        response = self.transport.submit(
            self.o, request.operation, allowance_us=1_000_000
        )
        self.assertEqual(response.code, 65, response)

    def query(self, request):
        response = self.transport.query(
            self.o, request.operation, allowance_us=1_000_000
        )
        self.assertEqual(response.code, 69, response)
        return cbor2.loads(response.payload)

    def succeeded(self, request):
        deadline = time.monotonic() + 5
        latest = None
        while time.monotonic() < deadline:
            latest = self.query(request)
            self.assertNotIn(latest[5], (2, 3), latest)
            if latest[5] == 1:
                return latest
            time.sleep(0.005)
        self.fail(f"operation did not succeed: {latest}")

    def marker(self, request, count):
        deadline = time.monotonic() + 5
        operation = request.operation.value
        while self.diagnostics.get(operation, 0) < count:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                self.fail(
                    f"missing guest marker {operation}/{count}: {self.diagnostics}"
                )
            try:
                _, line = self.lines.get(timeout=remaining)
            except queue.Empty:
                self.fail(
                    f"missing guest marker {operation}/{count}: {self.diagnostics}"
                )
            fields = line.split()
            if len(fields) == 3 and fields[0] == "ERROR":
                source, events = int(fields[1]), int(fields[2])
                self.diagnostics[source] = self.diagnostics.get(source, 0) + events

    def add_node(self, owner, logical, receiver):
        request = self.o.add_node(
            Node(
                logical,
                owner,
                b"traffic",
                self.size,
                self.crc,
                131072,
                bytes([receiver]),
                2_000_000,
                1_000_000,
                (Port(2 if receiver else 1, 0 if receiver else 1, 3, 4),),
            )
        )
        self.submit(request)
        self.succeeded(request)
        return request

    def exercise(self, remote):
        self.start(remote)
        source = self.add_node(1, 10, False)
        destination = self.add_node(2 if remote else 1, 20, True)
        channel = Channel(
            Endpoint(10, source.instance, 1, 1),
            Endpoint(20, destination.instance, 2 if remote else 1, 2),
            7,
            3,
            4,
            2,
            2_000_000,
        )
        addition = self.o.add_edge(channel, cleanup_us=1_000_000)
        self.submit(addition)
        added = self.succeeded(addition)
        self.assertEqual(len(added[7]), 2)
        self.assertTrue(all(facts & 3 == 3 for facts in added[7]), added)
        self.marker(destination, 1)  # Real payload crossed the selected placement.

        # Sustained source bursts exceed the small queues. Ordinary FULL/loss
        # must not turn the successful graph operation into a failure.
        time.sleep(0.05)
        steady = self.query(addition)
        self.assertEqual(steady[5], 1)
        self.assertTrue(all(facts & 4 == 0 for facts in steady[7]), steady)

        removal = self.o.remove_edge(addition.bindings[0], cleanup_us=1_000_000)
        self.submit(removal)
        removed = self.succeeded(removal)
        self.assertTrue(all(facts & 56 == 56 for facts in removed[7]), removed)
        # These markers are emitted by both still-running guests after they
        # consume disabled notifications, proving removal leaves peers alive.
        self.marker(source, 1)
        self.marker(destination, 2)
        for node in (source, destination):
            report = self.query(node)
            self.assertEqual(report[5], 1)
            self.assertEqual(report[7][0] & (4 | 8 | 16 | 32 | 64 | 128), 0, report)
            self.assertEqual(self.o.incidence(node.instance), ())
        historical = self.query(addition)
        self.assertEqual(historical[5], 1)
        self.assertTrue(all(facts & 32 for facts in historical[7]), historical)

    def test_local_edge_preserves_both_runtimes(self):
        self.exercise(False)

    def test_remote_edge_preserves_both_runtimes(self):
        self.exercise(True)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: p4_process_test.py HOST TRAFFIC_WASM")
    EdgeProcessTests.executable = str(Path(sys.argv[1]).resolve())
    EdgeProcessTests.artifact = Path(sys.argv[2]).read_bytes()
    unittest.main(argv=[sys.argv[0]], verbosity=2)
