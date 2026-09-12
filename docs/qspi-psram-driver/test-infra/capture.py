"""Capture a serial port for N seconds, surviving transient disconnects.

The ST-LINK VCP drops out while the target is reset or reflashed, which used
to kill the capture silently and produce an empty log.
"""

import sys
import time

import serial

port, out_path, seconds = sys.argv[1], sys.argv[2], float(sys.argv[3])
deadline = time.time() + seconds

with open(out_path, "wb") as out:
    while time.time() < deadline:
        try:
            with serial.Serial(port, 115200, timeout=0.2) as ser:
                while time.time() < deadline:
                    data = ser.read(4096)
                    if data:
                        out.write(data)
                        out.flush()
        except (serial.SerialException, OSError) as err:
            out.write(f"\n[capture: {err}, reopening]\n".encode())
            out.flush()
            time.sleep(0.5)
