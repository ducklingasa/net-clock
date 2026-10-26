#!/usr/bin/env python3
"""Export the application's packed weather icons as transparent PNG files."""

from __future__ import annotations

import re
import struct
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "main" / "weather_icons.h"
OUTPUT_DIRECTORY = ROOT / "docs"
PALETTE_PATTERN = re.compile(
    r"static const uint16_t weather_icon_palette\[(\d+)\]\s*=\s*\{(.*?)\};",
    re.DOTALL,
)
ICON_PATTERN = re.compile(
    r"static const uint8_t weather_icon_(\w+)_rle\[(\d+)\]\s*=\s*\{(.*?)\};",
    re.DOTALL,
)
HEX_BYTE_PATTERN = re.compile(r"0x([0-9A-Fa-f]{2})")
HEX_WORD_PATTERN = re.compile(r"0x([0-9A-Fa-f]{4})")


def read_application_icons() -> tuple[int, list[int], dict[str, list[int]]]:
    source = HEADER.read_text(encoding="ascii")
    size_match = re.search(r"#define WEATHER_ICON_SIZE (\d+)", source)
    palette_match = PALETTE_PATTERN.search(source)
    if size_match is None or palette_match is None:
        raise ValueError(f"Could not find icon size and palette in {HEADER}")

    icon_size = int(size_match.group(1))
    palette = [int(value, 16) for value in HEX_WORD_PATTERN.findall(palette_match.group(2))]
    if len(palette) != int(palette_match.group(1)):
        raise ValueError("The application icon palette length does not match its declaration")

    icons: dict[str, list[int]] = {}
    for match in ICON_PATTERN.finditer(source):
        name, declared_length, body = match.groups()
        data = [int(value, 16) for value in HEX_BYTE_PATTERN.findall(body)]
        if len(data) != int(declared_length):
            raise ValueError(f"Icon {name} data length does not match its declaration")
        if name in icons:
            raise ValueError(f"Duplicate application icon: {name}")
        icons[name] = data

    if not icons:
        raise ValueError(f"No packed application icons found in {HEADER}")
    return icon_size, palette, icons


def decode_icon(size: int, palette: list[int], encoded: list[int]) -> list[tuple[int, int, int, int]]:
    pixels = [(0, 0, 0, 0)] * (size * size)
    offset = 0

    for y in range(size):
        if offset >= len(encoded):
            raise ValueError(f"Icon data ended before row {y}")
        span_count = encoded[offset]
        offset += 1

        for _ in range(span_count):
            if offset + 2 > len(encoded):
                raise ValueError(f"Icon span header is incomplete on row {y}")
            start, length = encoded[offset : offset + 2]
            offset += 2
            if length == 0 or start + length > size or offset + length > len(encoded):
                raise ValueError(f"Icon span is out of bounds on row {y}")

            for x in range(start, start + length):
                palette_index = encoded[offset]
                offset += 1
                if not 1 <= palette_index <= len(palette):
                    raise ValueError(f"Invalid palette index {palette_index}")
                color = palette[palette_index - 1]
                if color == 0:
                    pixels[y * size + x] = (0, 0, 0, 0)
                else:
                    red = (color >> 11) & 0x1F
                    green = (color >> 5) & 0x3F
                    blue = color & 0x1F
                    pixels[y * size + x] = (
                        (red * 255 + 15) // 31,
                        (green * 255 + 31) // 63,
                        (blue * 255 + 15) // 31,
                        255,
                    )

    if offset != len(encoded):
        raise ValueError("Icon data contains unexpected trailing bytes")
    return pixels


def png_chunk(chunk_type: bytes, data: bytes) -> bytes:
    checksum = zlib.crc32(chunk_type + data) & 0xFFFFFFFF
    return struct.pack(">I", len(data)) + chunk_type + data + struct.pack(">I", checksum)


def write_png(path: Path, size: int, pixels: list[tuple[int, int, int, int]]) -> None:
    rows = bytearray()
    for y in range(size):
        rows.append(0)
        for red, green, blue, alpha in pixels[y * size : (y + 1) * size]:
            rows.extend((red, green, blue, alpha))

    png_data = bytearray(b"\x89PNG\r\n\x1a\n")
    png_data.extend(
        png_chunk(
            b"IHDR",
            struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0),
        )
    )
    png_data.extend(png_chunk(b"IDAT", zlib.compress(rows, level=9)))
    png_data.extend(png_chunk(b"IEND", b""))
    path.write_bytes(png_data)


def main() -> None:
    icon_size, palette, icons = read_application_icons()
    OUTPUT_DIRECTORY.mkdir(parents=True, exist_ok=True)

    for name, encoded in icons.items():
        output_path = OUTPUT_DIRECTORY / f"weather_{name}.png"
        write_png(output_path, icon_size, decode_icon(icon_size, palette, encoded))
        print(f"Wrote {output_path.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
