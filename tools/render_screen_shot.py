#!/usr/bin/env python3
"""Render 240x240 current-weather and forecast clock previews."""

from __future__ import annotations

import math
import re
import struct
import zlib
from datetime import datetime, timedelta
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN_SOURCE = ROOT / "main" / "main.c"
ICON_HEADER = ROOT / "main" / "weather_icons.h"
OUTPUT = ROOT / "docs" / "screen_shot.png"
FORECAST_OUTPUT = ROOT / "docs" / "screen_shot_forecast.png"
WIDTH = 240
HEIGHT = 240
CENTER = 120


def rgb565(red: int, green: int, blue: int) -> tuple[int, int, int]:
    value = ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3)
    return (
        ((((value >> 11) & 0x1F) * 255) + 15) // 31,
        ((((value >> 5) & 0x3F) * 255) + 31) // 63,
        (((value & 0x1F) * 255) + 15) // 31,
    )


def expand_rgb565(value: int) -> tuple[int, int, int]:
    red = (value >> 11) & 0x1F
    green = (value >> 5) & 0x3F
    blue = value & 0x1F
    return (
        (red * 255 + 15) // 31,
        (green * 255 + 31) // 63,
        (blue * 255 + 15) // 31,
    )


class Screen:
    def __init__(self) -> None:
        self.pixels = bytearray(WIDTH * HEIGHT * 3)

    def pixel(self, x: int, y: int, color: tuple[int, int, int]) -> None:
        if 0 <= x < WIDTH and 0 <= y < HEIGHT:
            offset = (y * WIDTH + x) * 3
            self.pixels[offset : offset + 3] = bytes(color)

    def line(
        self, x0: int, y0: int, x1: int, y1: int, color: tuple[int, int, int]
    ) -> None:
        dx, sx = abs(x1 - x0), (1 if x0 < x1 else -1)
        dy, sy = -abs(y1 - y0), (1 if y0 < y1 else -1)
        error = dx + dy
        while True:
            self.pixel(x0, y0, color)
            if x0 == x1 and y0 == y1:
                break
            doubled_error = 2 * error
            if doubled_error >= dy:
                error += dy
                x0 += sx
            if doubled_error <= dx:
                error += dx
                y0 += sy

    def circle(self, center_x: int, center_y: int, radius: int,
               color: tuple[int, int, int]) -> None:
        x, y, error = radius, 0, 1 - radius
        while x >= y:
            for px, py in (
                (center_x + x, center_y + y),
                (center_x + y, center_y + x),
                (center_x - y, center_y + x),
                (center_x - x, center_y + y),
                (center_x - x, center_y - y),
                (center_x - y, center_y - x),
                (center_x + y, center_y - x),
                (center_x + x, center_y - y),
            ):
                self.pixel(px, py, color)
            y += 1
            if error < 0:
                error += 2 * y + 1
            else:
                x -= 1
                error += 2 * (y - x) + 1

    def fill_circle(self, center_x: int, center_y: int, radius: int,
                    color: tuple[int, int, int]) -> None:
        for y in range(-radius, radius + 1):
            half_width = int(math.sqrt(radius * radius - y * y))
            self.line(
                center_x - half_width, center_y + y,
                center_x + half_width, center_y + y, color,
            )

    def text(self, text: str, x: int, y: int, scale: int,
             color: tuple[int, int, int],
             font: dict[str, list[int]]) -> None:
        for character in text:
            glyph = font.get(character)
            if glyph is not None:
                for column in range(5):
                    for row in range(7):
                        if glyph[column] & (1 << row):
                            for dy in range(scale):
                                for dx in range(scale):
                                    self.pixel(
                                        x + column * scale + dx,
                                        y + row * scale + dy,
                                        color,
                                    )
            x += 6 * scale

    def forecast_text(self, text: str, x: int, y: int, scale: int,
                      color: tuple[int, int, int],
                      font: dict[str, list[int]]) -> None:
        for character in text:
            glyph = font.get(character)
            if glyph is not None:
                for row in range(7):
                    for column in range(4):
                        if glyph[row] & (1 << (3 - column)):
                            for dy in range(scale):
                                for dx in range(scale):
                                    self.pixel(
                                        x + column * scale + dx,
                                        y + row * scale + dy,
                                        color,
                                    )
            x += 5 * scale

    def png(self) -> bytes:
        rows = bytearray()
        row_size = WIDTH * 3
        for y in range(HEIGHT):
            rows.append(0)
            start = y * row_size
            rows.extend(self.pixels[start : start + row_size])

        def chunk(kind: bytes, data: bytes) -> bytes:
            checksum = zlib.crc32(kind + data) & 0xFFFFFFFF
            return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", checksum)

        result = bytearray(b"\x89PNG\r\n\x1a\n")
        result.extend(chunk(b"IHDR", struct.pack(">IIBBBBB", WIDTH, HEIGHT, 8, 2, 0, 0, 0)))
        result.extend(chunk(b"IDAT", zlib.compress(rows, level=9)))
        result.extend(chunk(b"IEND", b""))
        return bytes(result)


def read_font() -> dict[str, list[int]]:
    source = MAIN_SOURCE.read_text(encoding="ascii")
    match = re.search(
        r'static const char font_characters\[\] = "([^"]+)";\s*'
        r"static const uint8_t font5x7\[\]\[5\] = \{(.*?)\n\};",
        source,
        re.DOTALL,
    )
    if match is None:
        raise ValueError(f"Could not find the application font in {MAIN_SOURCE}")
    characters, body = match.groups()
    glyphs = [
        [int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]{2})", row)]
        for row in re.findall(r"\{([^{}]+)\}", body)
    ]
    if len(glyphs) != len(characters) or any(len(glyph) != 5 for glyph in glyphs):
        raise ValueError("Application font data does not match its declaration")
    return dict(zip(characters, glyphs))


def read_forecast_font() -> dict[str, list[int]]:
    source = MAIN_SOURCE.read_text(encoding="ascii")
    match = re.search(
        r'static const char forecast_font4x7_characters\[\] = "([^"]+)";\s*'
        r"static const uint8_t forecast_font4x7\[\]\[7\] = \{(.*?)\n\};",
        source,
        re.DOTALL,
    )
    if match is None:
        raise ValueError(f"Could not find the forecast font in {MAIN_SOURCE}")
    characters, body = match.groups()
    glyphs = [
        [int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]+)", row)]
        for row in re.findall(r"\{([^{}]+)\}", body)
    ]
    if len(glyphs) != len(characters) or any(len(glyph) != 7 for glyph in glyphs):
        raise ValueError("Forecast font data does not match its declaration")
    return dict(zip(characters, glyphs))


def read_icon(icon_name: str) -> tuple[int, list[int], list[int]]:
    source = ICON_HEADER.read_text(encoding="ascii")
    size_match = re.search(r"#define WEATHER_ICON_SIZE (\d+)", source)
    palette_match = re.search(
        r"static const uint16_t weather_icon_palette\[\d+\]\s*=\s*\{(.*?)\};",
        source,
        re.DOTALL,
    )
    icon_match = re.search(
        rf"static const uint8_t weather_icon_{re.escape(icon_name)}_rle"
        r"\[(\d+)\]\s*=\s*\{(.*?)\};",
        source,
        re.DOTALL,
    )
    if size_match is None or palette_match is None or icon_match is None:
        raise ValueError(f"Could not find weather icon {icon_name!r} in {ICON_HEADER}")

    palette = [
        int(value, 16)
        for value in re.findall(r"0x([0-9A-Fa-f]{4})", palette_match.group(1))
    ]
    encoded = [
        int(value, 16)
        for value in re.findall(r"0x([0-9A-Fa-f]{2})", icon_match.group(2))
    ]
    if len(encoded) != int(icon_match.group(1)):
        raise ValueError(f"Icon {icon_name!r} data does not match its declared length")
    return int(size_match.group(1)), palette, encoded


def interpolate_rgb565(
    top_left: int,
    top_right: int,
    bottom_left: int,
    bottom_right: int,
    x_weight: float,
    y_weight: float,
) -> int:
    weights = (
        (1 - x_weight) * (1 - y_weight),
        x_weight * (1 - y_weight),
        (1 - x_weight) * y_weight,
        x_weight * y_weight,
    )
    colors = (top_left, top_right, bottom_left, bottom_right)
    channels = []
    for shift, mask in ((11, 0x1F), (5, 0x3F), (0, 0x1F)):
        value = sum(((color >> shift) & mask) * weight
                    for color, weight in zip(colors, weights))
        channels.append(math.floor(value + 0.5))
    return (channels[0] << 11) | (channels[1] << 5) | channels[2]


def draw_icon(
    screen: Screen,
    x: int,
    y: int,
    icon_name: str,
    output_size: int | None = None,
) -> None:
    size, palette, encoded = read_icon(icon_name)
    source_pixels = [0] * (size * size)
    offset = 0
    for row in range(size):
        if offset >= len(encoded):
            raise ValueError(f"Icon {icon_name!r} data ended before row {row}")
        span_count = encoded[offset]
        offset += 1
        for _ in range(span_count):
            if offset + 2 > len(encoded):
                raise ValueError(f"Icon {icon_name!r} span is incomplete on row {row}")
            start, length = encoded[offset : offset + 2]
            offset += 2
            if length == 0 or start + length > size or offset + length > len(encoded):
                raise ValueError(f"Icon {icon_name!r} span is out of bounds on row {row}")
            for column in range(start, start + length):
                color_index = encoded[offset]
                offset += 1
                if not 1 <= color_index <= len(palette):
                    raise ValueError(
                        f"Invalid palette index {color_index} in icon {icon_name!r}"
                    )
                color = palette[color_index - 1]
                if color:
                    source_pixels[row * size + column] = color
    if offset != len(encoded):
        raise ValueError(f"Icon {icon_name!r} data contains unexpected trailing bytes")
    output_size = size if output_size is None else output_size
    for row in range(output_size):
        source_y = row * (size - 1) / (output_size - 1)
        y0 = int(source_y)
        y1 = min(y0 + 1, size - 1)
        y_weight = source_y - y0
        for column in range(output_size):
            source_x = column * (size - 1) / (output_size - 1)
            x0 = int(source_x)
            x1 = min(x0 + 1, size - 1)
            x_weight = source_x - x0
            color = interpolate_rgb565(
                source_pixels[y0 * size + x0],
                source_pixels[y0 * size + x1],
                source_pixels[y1 * size + x0],
                source_pixels[y1 * size + x1],
                x_weight,
                y_weight,
            )
            if color:
                screen.pixel(x + column, y + row, expand_rgb565(color))


def text_width(text: str, scale: int) -> int:
    return 0 if not text else (len(text) * 6 - 1) * scale


def forecast_text_width(text: str, scale: int) -> int:
    return 0 if not text else (len(text) * 5 - 1) * scale


def draw_forecast_hour_marker(screen: Screen, tick: int,
                              color: tuple[int, int, int]) -> None:
    angle = (tick * 6.0 - 90.0) * math.pi / 180.0
    dx, dy = math.cos(angle), math.sin(angle)
    left = CENTER + math.floor(min(101 * dx, 112 * dx)) - 2
    right = CENTER + math.ceil(max(101 * dx, 112 * dx)) + 2
    top = CENTER + math.floor(min(101 * dy, 112 * dy)) - 2
    bottom = CENTER + math.ceil(max(101 * dy, 112 * dy)) + 2
    for y in range(top, bottom + 1):
        for x in range(left, right + 1):
            along = (x - CENTER) * dx + (y - CENTER) * dy
            across = -(x - CENTER) * dy + (y - CENTER) * dx
            if 101 <= along <= 112 and abs(across) < 1.5:
                screen.pixel(x, y, color)


def draw_screen(
    date_text: str,
    time_text: str,
    second: int,
    temperature: int,
    weather_icon: str,
    humidity: int,
    colon_white: bool = True,
) -> Screen:
    if not 0 <= second < 60:
        raise ValueError("Seconds must be between 0 and 59")

    screen = Screen()
    font = read_font()
    dial_color = rgb565(90, 90, 100)
    screen.circle(CENTER, CENTER, 118, dial_color)
    screen.circle(CENTER, CENTER, 117, dial_color)

    for tick in range(60):
        angle = (tick * 6.0 - 90.0) * math.pi / 180.0
        elapsed = second == 0 or 0 < tick <= second
        color = (
            rgb565(0, 255, 30) if tick % 5 == 0
            else rgb565(255, 255, 255)
        ) if elapsed else dial_color
        if tick % 5 == 0:
            dx, dy = math.cos(angle), math.sin(angle)
            left = CENTER + math.floor(min(101 * dx, 112 * dx)) - 2
            right = CENTER + math.ceil(max(101 * dx, 112 * dx)) + 2
            top = CENTER + math.floor(min(101 * dy, 112 * dy)) - 2
            bottom = CENTER + math.ceil(max(101 * dy, 112 * dy)) + 2
            for y in range(top, bottom + 1):
                for x in range(left, right + 1):
                    along = (x - CENTER) * dx + (y - CENTER) * dy
                    across = -(x - CENTER) * dy + (y - CENTER) * dx
                    if 101 <= along <= 112 and abs(across) < 1.5:
                        screen.pixel(x, y, color)
        else:
            x1 = CENTER + int(math.cos(angle) * 107)
            y1 = CENTER + int(math.sin(angle) * 107)
            x2 = CENTER + int(math.cos(angle) * 112)
            y2 = CENTER + int(math.sin(angle) * 112)
            if elapsed:
                offset_x = int(math.copysign(
                    math.floor(abs(math.sin(angle)) + 0.5), -math.sin(angle)
                ))
                offset_y = int(math.copysign(
                    math.floor(abs(math.cos(angle)) + 0.5), math.cos(angle)
                ))
                for offset in (-1, 0, 1):
                    screen.line(
                        x1 + offset * offset_x, y1 + offset * offset_y,
                        x2 + offset * offset_x, y2 + offset * offset_y, color,
                    )
            else:
                screen.line(x1, y1, x2, y2, color)

    date_text = date_text.upper()
    screen.text(
        date_text, (WIDTH - text_width(date_text, 3)) // 2, 43, 3,
        rgb565(0, 225, 240), font,
    )
    screen.text(
        time_text, (WIDTH - text_width(time_text, 6)) // 2, 78, 6,
        rgb565(255, 255, 255), font,
    )
    if not colon_white:
        colon_x = (WIDTH - text_width(time_text, 6)) // 2 + 2 * 6 * 6
        screen.text(":", colon_x, 78, 6, dial_color, font)
    draw_icon(screen, 54, 128, weather_icon)

    temperature_text = str(temperature)
    temperature_scale, temperature_x, temperature_y = 4, 119, 139
    if len(temperature_text) > 3:
        temperature_scale = 3
    red = rgb565(255, 20, 20)
    screen.text(
        temperature_text, temperature_x, temperature_y, temperature_scale, red, font
    )
    degree_x = temperature_x + text_width(temperature_text, temperature_scale) + 5
    screen.fill_circle(degree_x, temperature_y + 4, 4, red)
    screen.fill_circle(degree_x, temperature_y + 4, 2, rgb565(0, 0, 0))
    screen.text("C", degree_x + 8, temperature_y + 7, 3, red, font)

    humidity_text, humidity_scale = f"{humidity}%", 3
    humidity_width = text_width(humidity_text, humidity_scale)
    humidity_start = (WIDTH - (humidity_width + 27)) // 2
    blue = rgb565(0, 90, 255)
    for row in range(25):
        half_width = (
            9.0 * math.sin(row * 3.14159265 / 30.0)
            if row <= 15
            else math.sqrt(81.0 - (row - 15) * (row - 15))
        )
        width = math.floor(half_width + 0.5)
        screen.line(
            humidity_start + 9 - width, 180 + row,
            humidity_start + 9 + width, 180 + row, blue,
        )
    screen.line(humidity_start + 4, 195, humidity_start + 4, 198, rgb565(100, 180, 255))
    screen.pixel(humidity_start + 5, 199, rgb565(100, 180, 255))
    screen.text(humidity_text, humidity_start + 27, 183, humidity_scale, blue, font)
    return screen


def draw_forecast_screen(
    forecast_time: datetime,
    forecast_icons: tuple[str, ...],
) -> Screen:
    if len(forecast_icons) != 12:
        raise ValueError("The forecast preview requires 12 hourly icons")

    screen = Screen()
    font = read_font()
    forecast_font = read_forecast_font()
    dial_color = rgb565(90, 90, 100)
    green = rgb565(0, 255, 30)
    screen.circle(CENTER, CENTER, 118, dial_color)
    screen.circle(CENTER, CENTER, 117, dial_color)

    first_hour = forecast_time.replace(minute=0, second=0, microsecond=0)
    for index, icon_name in enumerate(forecast_icons):
        forecast_hour = first_hour + timedelta(hours=index)
        angle = (index * 30.0 - 90.0) * math.pi / 180.0
        hour_text = forecast_hour.strftime("%H")
        label_x = CENTER + round(math.cos(angle) * 84)
        label_y = CENTER + round(math.sin(angle) * 84)
        screen.text(
            hour_text,
            label_x - text_width(hour_text, 1) // 2,
            label_y - 3,
            1,
            rgb565(190, 190, 195),
            font,
        )

        icon_angle = (index * 30.0 + 15.0 - 90.0) * math.pi / 180.0
        icon_x = CENTER + round(math.cos(icon_angle) * 97) - 16
        icon_y = CENTER + round(math.sin(icon_angle) * 97) - 16
        draw_icon(screen, icon_x, icon_y, icon_name, output_size=32)
        draw_forecast_hour_marker(screen, index * 5, green)

    time_text = forecast_time.strftime("%H:%M")
    weekday_text = forecast_time.strftime("%a").upper()
    date_text = forecast_time.strftime("%d %b").upper()
    time_scale = 6
    time_x = (WIDTH - forecast_text_width(time_text, time_scale)) // 2
    time_y = (HEIGHT - 7 * time_scale) // 2
    date_scale = 3
    date_gap = 12
    screen.text(
        weekday_text,
        (WIDTH - text_width(weekday_text, date_scale)) // 2,
        time_y - 7 * date_scale - date_gap,
        date_scale,
        rgb565(0, 225, 240),
        font,
    )
    screen.forecast_text(
        time_text, time_x, time_y, time_scale, rgb565(255, 255, 255), forecast_font
    )
    screen.text(
        date_text,
        (WIDTH - text_width(date_text, date_scale)) // 2,
        time_y + 7 * time_scale + date_gap,
        date_scale,
        rgb565(0, 225, 240),
        font,
    )
    return screen


def main(
    date_text: str,
    time_text: str,
    second: int,
    temperature: int,
    weather_icon: str,
    humidity: int,
    output: Path = OUTPUT,
    colon_white: bool = True,
    forecast_time: datetime = datetime(2026, 11, 11, 13, 25),
    forecast_icons: tuple[str, ...] = (
        "few_day", "few_day", "scattered_day", "broken_day", "rain_day",
        "sunset", "clear_night", "few_night", "few_night", "clear_night",
        "clear_night", "clear_night",
    ),
    forecast_output: Path = FORECAST_OUTPUT,
) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(
        draw_screen(
            date_text, time_text, second, temperature, weather_icon, humidity,
            colon_white=colon_white,
        ).png()
    )
    try:
        location = output.relative_to(ROOT)
    except ValueError:
        location = output
    print(f"Wrote {location}")
    forecast_output.parent.mkdir(parents=True, exist_ok=True)
    forecast_output.write_bytes(draw_forecast_screen(forecast_time, forecast_icons).png())
    try:
        location = forecast_output.relative_to(ROOT)
    except ValueError:
        location = forecast_output
    print(f"Wrote {location}")


if __name__ == "__main__":
    main(
        date_text="TUE 20",
        time_text="13:25",
        second=25,
        temperature=15,
        weather_icon="few_day",
        humidity=65,
        forecast_time=datetime(2026, 11, 11, 13, 25),
        forecast_icons=(
            "few_day", "few_day", "scattered_day", "broken_day", "rain_day",
            "sunset", "clear_night", "few_night", "few_night", "clear_night",
            "clear_night", "clear_night",
        ),
    )
