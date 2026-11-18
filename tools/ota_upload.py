#!/usr/bin/env python3
"""Upload the latest application build to a Network Clock over HTTP OTA."""

from __future__ import annotations

import getpass
import http.client
import ipaddress
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / "build" / "net_clock.bin"
CHUNK_SIZE = 64 * 1024


def main() -> int:
    if not FIRMWARE.is_file():
        print(f"Firmware image not found: {FIRMWARE}", file=sys.stderr)
        print("Build the project first with `idf.py build`.", file=sys.stderr)
        return 1

    try:
        device_ip = str(ipaddress.IPv4Address(input("OTA device IPv4 address: ").strip()))
    except ValueError:
        print("Enter a valid IPv4 address.", file=sys.stderr)
        return 2

    password = getpass.getpass("OTA password: ")
    if not 8 <= len(password) <= 63:
        print("The OTA password must be 8 to 63 characters.", file=sys.stderr)
        return 2
    if "\r" in password or "\n" in password:
        print("The OTA password cannot contain a newline.", file=sys.stderr)
        return 2

    firmware_size = FIRMWARE.stat().st_size
    firmware_name = FIRMWARE.relative_to(ROOT).as_posix()
    print(f"Uploading {firmware_name} ({firmware_size:,} bytes) to {device_ip}...")

    connection = http.client.HTTPConnection(device_ip, timeout=120)
    try:
        connection.putrequest("POST", "/update")
        connection.putheader("Content-Type", "application/octet-stream")
        connection.putheader("Content-Length", str(firmware_size))
        connection.putheader("X-OTA-Password", password)
        connection.endheaders()

        uploaded = 0
        with FIRMWARE.open("rb") as firmware:
            while chunk := firmware.read(CHUNK_SIZE):
                connection.send(chunk)
                uploaded += len(chunk)
                percent = uploaded * 100 // firmware_size
                print(f"\rUpload progress: {percent:3d}%", end="", flush=True)
        print()

        response = connection.getresponse()
        body = response.read().decode("utf-8", errors="replace").strip()
        if response.status != 200:
            print(f"OTA failed: HTTP {response.status} {response.reason}", file=sys.stderr)
            if body:
                print(body, file=sys.stderr)
            return 1

        print(body or "OTA update accepted; the device is restarting.")
        return 0
    except (OSError, http.client.HTTPException, ValueError) as error:
        print(f"Could not upload firmware: {error}", file=sys.stderr)
        return 1
    finally:
        connection.close()


if __name__ == "__main__":
    raise SystemExit(main())