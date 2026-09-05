"""
Usage:
    pip install pyserial
    python controller_bridge.py --port /dev/ttyACM0
    (Windows: --port COM5, macOS: --port /dev/cu.usbmodemXXXX)
"""

import argparse
import socket
import sys

import serial

UDP_IP = "127.0.0.1"
UDP_PORT = 5556  # separate from GestureInput's 5555

FRAME_START = 0xFF
FRAME_LEN = 7  # start, x1, y1, x2, y2, buttons, checksum


def read_frame(ser: serial.Serial) -> bytes | None:
    """Blocks until it finds a valid, checksum-passing frame, or returns
    None if the read times out first."""
    b = ser.read(1)
    if not b or b[0] != FRAME_START:
        return None

    rest = ser.read(FRAME_LEN - 1)
    if len(rest) != FRAME_LEN - 1:
        return None

    x1, y1, x2, y2, buttons, checksum = rest
    if (x1 ^ y1 ^ x2 ^ y2 ^ buttons) != checksum:
        return None  # corrupted frame, drop it -- next 0xFF resyncs us

    return bytes([x1, y1, x2, y2, buttons])


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True, help="Serial port, e.g. /dev/ttyACM0 or COM5")
    parser.add_argument("--baud", type=int, default=115200)
    args = parser.parse_args()

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.5)
    except serial.SerialException as e:
        print(f"controller_bridge: couldn't open {args.port}: {e}", file=sys.stderr)
        sys.exit(1)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    print(f"controller_bridge: forwarding {args.port} -> {UDP_IP}:{UDP_PORT} (Ctrl+C to stop)")
    try:
        while True:
            payload = read_frame(ser)
            if payload is not None:
                sock.sendto(payload, (UDP_IP, UDP_PORT))
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
        sock.close()


if __name__ == "__main__":
    main()
