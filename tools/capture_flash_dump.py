"""
capture_flash_dump.py -- robust raw-byte serial capture for retrieving a
peripheral's CONFIG_APP_DUMP_ON_BOOT CSV output (see BUILD_AND_FLASH.md's
"Retrieving a node's full on-board flash log" section).

Why this exists instead of tools/Watch-SerialLog.ps1: that tool reads with
.NET SerialPort.ReadLine(), which is line-buffered/blocking -- confirmed on
real hardware (2026-08-13, node 5, 1152-row dump) to silently lose roughly
half the rows during a fast burst, even with the firmware's own throttling
(~10ms/row, specifically added to avoid overrunning the console's own
buffer). This script instead does a continuous raw byte-stream read
(pyserial's Serial.read(), no line buffering) and writes straight to disk,
which caught the same dump completely (1167/1167 rows, contiguous, on a
second attempt) with the same firmware and hardware.

Also reconnect-resilient: a board reset briefly drops and re-enumerates its
USB CDC-ACM port, which would kill a naive single-open loop mid-capture.

Usage:
    python capture_flash_dump.py COM<port> <duration_seconds> <output_file>

Then parse the output file for the dump firmware's CSV lines
("<node_id>,<seq>,0x<flags>,<row>,<temp_c>,<humidity_pct>") and check the
row count against the dump's own trailer line ("# <N> rows") -- the parsed
`row` column should be a contiguous 0..N-1 sequence with no gaps. If it
isn't, the capture is still incomplete; increase the duration and re-run
(a fresh capture needs a fresh reset of the board to re-trigger the dump).
"""
import sys
import time

import serial


def main():
    if len(sys.argv) < 4:
        print(f"Usage: {sys.argv[0]} <COM_PORT> <DURATION_SECONDS> <OUTPUT_FILE>")
        sys.exit(1)

    port = sys.argv[1]
    duration_s = float(sys.argv[2])
    out_path = sys.argv[3]

    print(f"Capturing {port} for {duration_s}s -> {out_path}")

    total = 0
    deadline = time.time() + duration_s

    with open(out_path, "wb") as f:
        while time.time() < deadline:
            try:
                ser = serial.Serial(port, baudrate=115200, timeout=0.2)
            except serial.SerialException:
                time.sleep(0.3)
                continue

            try:
                while time.time() < deadline:
                    chunk = ser.read(4096)
                    if chunk:
                        f.write(chunk)
                        f.flush()
                        total += len(chunk)
            except (serial.SerialException, OSError):
                pass
            finally:
                try:
                    ser.close()
                except Exception:
                    pass

    print(f"Done. {total} bytes captured.")


if __name__ == "__main__":
    main()
