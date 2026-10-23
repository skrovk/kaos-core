"""Authorized native imports through real request, artifact and runtime paths."""

import sys
import tempfile
import unittest
from pathlib import Path

from sim_env.graph_protocol import Node
from sim_env.graph_run import GraphRun
from sim_env.graph_transport import CREATED

HOST, GUESTS = Path(sys.argv.pop(1)), Path(sys.argv.pop(1))
GRANT = ("fixture_temperature_read",)


class NativeProcessTests(unittest.TestCase):
    def rejected_next_to_granted(self, artifact, grants):
        with (
            tempfile.TemporaryDirectory(prefix="kaos-native-process-") as directory,
            GraphRun(HOST, Path(directory) / "run", (1,)) as run,
        ):
            good = (GUESTS / "native_sensor.wasm").read_bytes()
            bad = (GUESTS / artifact).read_bytes()
            good_size, good_crc = run.add_artifact(b"good", good)
            bad_size, bad_crc = run.add_artifact(b"bad", bad)

            def submit(node):
                request = run.o.add_node(node)
                self.assertEqual(run.submit(request).code, CREATED)
                return request, run.wait(request)

            live, started = submit(
                Node(
                    10,
                    1,
                    b"good",
                    good_size,
                    good_crc,
                    131072,
                    b"\0",
                    3_000_000,
                    2_000_000,
                    imports=GRANT,
                )
            )
            self.assertEqual(started[5], 1)
            self.assertEqual(started[6] & 39, 39)
            _, rejected = submit(
                Node(
                    20,
                    1,
                    b"bad",
                    bad_size,
                    bad_crc,
                    131072,
                    b"\0",
                    3_000_000,
                    2_000_000,
                    imports=grants,
                )
            )
            self.assertEqual(rejected[5], 2)  # Failed after full cleanup.
            self.assertEqual(rejected[6] & 39, 0)  # Never committed/started.
            self.assertEqual(rejected[7][0] & 56, 56)
            removal = run.o.remove_node(
                live.instance, cleanup_us=2_000_000, no_edges=True
            )
            self.assertEqual(run.submit(removal).code, CREATED)
            self.assertEqual(run.wait(removal)[5], 1)
            # Fresh incarnation cannot inherit the removed instance's grant.
            _, replacement = submit(
                Node(
                    10,
                    1,
                    b"good",
                    good_size,
                    good_crc,
                    131072,
                    b"\0",
                    3_000_000,
                    2_000_000,
                )
            )
            self.assertEqual(replacement[5], 2)
            self.assertEqual(replacement[6] & 39, 0)
            self.assertEqual(replacement[7][0] & 56, 56)

    def test_undeclared_import_cannot_use_neighbours_registration(self):
        self.rejected_next_to_granted("native_sensor.wasm", ())

    def test_unknown_grant_fails_before_start(self):
        self.rejected_next_to_granted("native_sensor.wasm", ("unknown",))

    def test_wrong_module_fails_before_start(self):
        self.rejected_next_to_granted("native_wrong_module.wasm", GRANT)

    def test_unknown_import_fails_before_start(self):
        self.rejected_next_to_granted("native_unknown.wasm", GRANT)

    def test_wrong_signature_fails_before_start(self):
        self.rejected_next_to_granted("native_wrong_signature.wasm", GRANT)

    def test_wrong_standard_signature_fails_before_start(self):
        self.rejected_next_to_granted("native_bad_standard.wasm", ())


if __name__ == "__main__":
    unittest.main()
