"""Reset the enrolled ESP32 and capture the bounded P4 qualification run."""

import sys
import time
from pathlib import Path

import serial


with serial.Serial("/dev/ttyUSB0", 115200, timeout=0.2) as board, Path(
    "/tmp/kaos-p4-session/board.log"
).open("wb") as log:
    board.dtr = False
    board.rts = True
    time.sleep(0.1)
    board.rts = False
    end = time.monotonic() + 90
    captured = bytearray()
    while time.monotonic() < end:
        chunk = board.read(4096)
        captured.extend(chunk)
        log.write(chunk)
        log.flush()
        sys.stdout.buffer.write(chunk)
        sys.stdout.buffer.flush()
        if b"P4 capacity PASS" in captured:
            break
        if b"abort()" in captured or b"Guru Meditation" in captured:
            raise SystemExit("ESP32 qualification failed")
    else:
        raise SystemExit("ESP32 qualification timed out")
