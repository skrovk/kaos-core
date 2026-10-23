"""Reset the ESP32 and capture the unmodified P7 qualification fixture."""
import sys
import time
from pathlib import Path

import serial

with serial.Serial('/dev/ttyUSB0', 115200, timeout=0.2) as board:
    board.dtr = False
    board.rts = True
    time.sleep(0.1)
    board.reset_input_buffer()
    board.rts = False
    deadline = time.monotonic() + 90
    captured = bytearray()
    with Path('/tmp/kaos-p7-session/board.log').open('wb') as log:
        while time.monotonic() < deadline:
            chunk = board.read(4096)
            captured.extend(chunk)
            log.write(chunk)
            log.flush()
            sys.stdout.buffer.write(chunk)
            sys.stdout.buffer.flush()
            if b'abort()' in captured or b'Guru Meditation' in captured:
                raise SystemExit('P7 board qualification failed')
            if b'P7 capacity PASS' in captured:
                break
        else:
            raise SystemExit('P7 board qualification timed out')
