#!/usr/bin/env python3
"""Send a raw JetDirect (TCP 9100) job to a printer or the print-bridge board."""

from __future__ import annotations

import argparse
import socket
import sys
from pathlib import Path

DEFAULT_ZPL = b"^XA^FO50,50^A0N,40,40^FDHELLO^FS^XZ\n"
FIXTURES = Path(__file__).resolve().parent / "fixtures"


def load_payload(path: Path | None, inline_zpl: bool) -> bytes:
    if path is not None:
        data = path.read_bytes()
        if not data:
            raise SystemExit(f"empty file: {path}")
        return data
    if inline_zpl:
        return DEFAULT_ZPL
    default_fixture = FIXTURES / "hello.zpl"
    if default_fixture.is_file():
        return default_fixture.read_bytes()
    return DEFAULT_ZPL


def send_job(host: str, port: int, payload: bytes, timeout: float) -> None:
    print(f"connecting to {host}:{port} ({len(payload)} bytes)...")
    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        sock.sendall(payload)
        # Some devices half-close; do not require a response.
    print("sent ok")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, help="Board or printer IP / hostname")
    parser.add_argument("--port", type=int, default=9100, help="TCP port (default 9100)")
    parser.add_argument(
        "--file",
        type=Path,
        help="Raw job file (ZPL/EPL/TSPL). Default: scripts/fixtures/hello.zpl",
    )
    parser.add_argument(
        "--inline-zpl",
        action="store_true",
        help="Send a tiny built-in ZPL HELLO instead of a fixture file",
    )
    parser.add_argument("--timeout", type=float, default=10.0, help="Connect/send timeout seconds")
    args = parser.parse_args()

    payload = load_payload(args.file, args.inline_zpl)
    try:
        send_job(args.host, args.port, payload, args.timeout)
    except OSError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
