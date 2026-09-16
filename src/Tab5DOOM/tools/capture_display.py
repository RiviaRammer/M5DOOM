"""Bounded, passive Tab5 serial capture; never sends data or requests a reset."""

import argparse
from datetime import datetime
from pathlib import Path
import re
import time


def summarize(text):
    text = re.sub(r"\x1b\[[0-9;]*m", "", text)
    lines = text.splitlines()
    # ISR logging can interleave with task logging, even without a newline.
    underruns = text.count("underrun happens")
    stats = [line for line in lines if "tab5_lcd:" in line]
    return underruns, stats


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="Tab5 serial port, e.g. COM7")
    parser.add_argument("--seconds", type=float, default=15)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not 0 < args.seconds <= 300:
        parser.error("--seconds must be greater than zero and at most 300")

    import serial  # Available in the ESP-IDF Python environment.

    output = args.output or (Path(__file__).resolve().parents[1] / "build" /
                             ("serial-" + datetime.now().strftime("%Y%m%d-%H%M%S-%f") + ".log"))
    # Open first, with exclusive creation, so failed output setup never opens the port.
    output.parent.mkdir(parents=True, exist_ok=True)
    data = bytearray()
    port = serial.Serial(port=None, baudrate=115200, timeout=0.25)
    port.dtr = False
    port.rts = False
    port.port = args.port
    with output.open("xb") as log:
        try:
            port.open()
            start = time.monotonic()
            print(f"Passive capture: {args.port}, 115200 baud, {args.seconds:g}s", flush=True)
            while time.monotonic() - start < args.seconds:
                chunk = port.read(min(port.in_waiting or 1, 65536))
                log.write(chunk)
                data.extend(chunk)
                if len(data) > 8 * 1024 * 1024:
                    raise RuntimeError("Capture exceeded the 8 MiB safety limit")
        finally:
            port.close()
    underruns, stats = summarize(data.decode("utf-8", errors="replace"))
    print(f"Received {len(data)} bytes; LCD underruns: {underruns}")
    print("\n".join(stats) if stats else "No tab5_lcd statistics received.")
    print(f"Log: {output}")


if __name__ == "__main__":
    main()
