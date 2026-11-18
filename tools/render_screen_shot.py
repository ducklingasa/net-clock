#!/usr/bin/env python3
"""Render 240x240 current-weather and forecast clock previews."""

from __future__ import annotations

import math
import re
import struct
import zlib
from datetime import datetime, timedelta
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FONT_SOURCE = ROOT / "main" / "font.c"
ICON_HEADER = ROOT / "main" / "weather_icons.h"
OUTPUT = ROOT / "docs" / "screen_shot.png"
FORECAST_OUTPUT = ROOT / "docs" / "screen_shot_forecast.png"
WIDTH = 240
HEIGHT = 240
CENTER = 120


@dataclass
class BakedFont:
    characters: str
    alpha: bytes
    stride: int
    width: int
    height: int
    phases: int
    advance_half: int
    colon_adjust_half: int


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

    def text(self, text: str, x: int, y: int,
             color: tuple[int, int, int], font: BakedFont) -> None:
        offset_half = 0
        for character in text:
            if character in font.characters:
                phase = offset_half % 2 if font.phases == 2 else 0
                offset = (font.characters.index(character) * font.phases + phase) * font.stride
                for row in range(font.height):
                    for column in range(font.width):
                        pixel = row * font.width + column
                        packed = font.alpha[offset + pixel // 2]
                        alpha = ((packed & 15) if pixel % 2 else (packed >> 4)) * 17
                        pixel_x, pixel_y = x + offset_half // 2 + column, y + row
                        if alpha == 255:
                            self.pixel(pixel_x, pixel_y, color)
                        elif alpha and 0 <= pixel_x < WIDTH and 0 <= pixel_y < HEIGHT:
                            background_offset = (pixel_y * WIDTH + pixel_x) * 3
                            background = self.pixels[background_offset:background_offset + 3]
                            channels = []
                            for back, fore, shift in zip(background, color, (3, 2, 3)):
                                channels.append(((back >> shift) * (255 - alpha) +
                                                 (fore >> shift) * alpha + 127) // 255)
                            self.pixel(pixel_x, pixel_y, expand_rgb565(
                                (channels[0] << 11) | (channels[1] << 5) | channels[2]
                            ))
            offset_half += font.advance_half - (
                font.colon_adjust_half if character == ":" else 0
            )

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


def decode_c_string(literal: str) -> str:
    encoded = bytearray()
    index = 1
    while index < len(literal) - 1:
        character = literal[index]
        if character != "\\":
            encoded.append(ord(character))
            index += 1
            continue
        index += 1
        if index < len(literal) - 1 and literal[index] in "01234567":
            end = index
            while end < min(index + 3, len(literal) - 1) and literal[end] in "01234567":
                end += 1
            encoded.append(int(literal[index:end], 8))
            index = end
        else:
            escapes = {"\\": 0x5C, '"': 0x22, "n": 0x0A, "r": 0x0D, "t": 0x09}
            if index >= len(literal) - 1 or literal[index] not in escapes:
                raise ValueError(f"Unsupported escape in C string {literal!r}")
            encoded.append(escapes[literal[index]])
            index += 1
    return encoded.decode("utf-8")


def read_font() -> dict[str, BakedFont]:
    source = FONT_SOURCE.read_text(encoding="ascii")
    arrays = {
        name: bytes(int(value, 16) for value in re.findall(r"0x([0-9A-F]{2})", body))
        for name, body in re.findall(
            r"static const uint8_t (font_\w+_alpha)\[\] = \{(.*?)\n\};", source, re.DOTALL
        )
    }
    descriptors = re.findall(
        r'const clock_font_t clock_font_(\w+)\s*=\s*\{\s*'
        r'("(?:\\.|[^"\\])*")\s*,\s*(font_\w+_alpha)\s*,\s*'
        r'(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*\}',
        source,
    )
    fonts: dict[str, BakedFont] = {}
    for font_name, literal, array_name, *metrics in descriptors:
        baked = BakedFont(decode_c_string(literal), arrays[array_name],
                          *(int(value) for value in metrics))
        if len(baked.alpha) != len(baked.characters) * baked.phases * baked.stride:
            raise ValueError(f"Invalid font data length for {array_name}")
        fonts[font_name] = baked
    expected_fonts = {"small", "date", "humidity", "temperature", "time", "narrow_time"}
    if fonts.keys() != expected_fonts:
        raise ValueError(f"Expected fonts {sorted(expected_fonts)}, found {sorted(fonts)}")
    return fonts


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


def text_width(text: str, font: BakedFont) -> int:
    return 0 if not text else math.floor(0.5 * (
        (len(text) - 1) * font.advance_half + 2 * font.width -
        text.count(":") * font.colon_adjust_half
    ) + 0.5)


def make_text_placeholder(text: str, character: str) -> str:
    return "".join(value if value in " :" else character for value in text)


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
    inactive_segment_color = rgb565(45, 45, 50)
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
    date_font = font["date"]
    date_x = (WIDTH - text_width(date_text, date_font)) // 2
    screen.text(make_text_placeholder(date_text, "~"), date_x, 43,
                inactive_segment_color, date_font)
    screen.text(date_text, date_x, 43, rgb565(0, 225, 240), date_font)
    time_font = font["time"]
    time_x = (WIDTH - text_width(time_text, time_font)) // 2
    time_placeholder = make_text_placeholder(time_text, "8")
    screen.text(time_placeholder, time_x, 78, inactive_segment_color, time_font)
    screen.text(time_text, time_x, 78, rgb565(255, 255, 255), time_font)
    if not colon_white:
        colon_x = time_x + time_font.advance_half
        screen.text(":", colon_x, 78, dial_color, time_font)
    draw_icon(screen, 44, 128, weather_icon)

    temperature_value = str(temperature)
    temperature_font = font["date"] if len(temperature_value) > 2 else font["temperature"]
    temperature_text = f"{temperature_value}\u00b0C"
    red = rgb565(255, 20, 20)
    screen.text(temperature_text, 109, 139, red, temperature_font)

    humidity_text = f"{humidity}%"
    humidity_font = font["humidity"]
    humidity_width = text_width(humidity_text, humidity_font)
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
    screen.text(humidity_text, humidity_start + 27, 183, blue, humidity_font)
    return screen


def draw_forecast_screen(
    forecast_time: datetime,
    forecast_icons: tuple[str, ...],
) -> Screen:
    if len(forecast_icons) != 12:
        raise ValueError("The forecast preview requires 12 hourly icons")

    screen = Screen()
    font = read_font()
    dial_color = rgb565(90, 90, 100)
    inactive_segment_color = rgb565(45, 45, 50)
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
        hour_font = font["small"]
        screen.text(
            hour_text,
            label_x - text_width(hour_text, hour_font) // 2,
            label_y - hour_font.height // 2,
            rgb565(190, 190, 195),
            hour_font,
        )

        icon_angle = (index * 30.0 + 15.0 - 90.0) * math.pi / 180.0
        icon_x = CENTER + round(math.cos(icon_angle) * 97) - 16
        icon_y = CENTER + round(math.sin(icon_angle) * 97) - 16
        draw_icon(screen, icon_x, icon_y, icon_name, output_size=32)
        draw_forecast_hour_marker(screen, index * 5, green)

    time_text = forecast_time.strftime("%H:%M")
    weekday_text = forecast_time.strftime("%a").upper()
    date_text = forecast_time.strftime("%d %b").upper()
    time_font = font["narrow_time"]
    date_font = font["date"]
    time_x = (WIDTH - text_width(time_text, time_font)) // 2
    time_y = (HEIGHT - time_font.height) // 2
    date_gap = 12
    weekday_x = (WIDTH - text_width(weekday_text, date_font)) // 2
    weekday_y = time_y - date_font.height - date_gap
    screen.text(make_text_placeholder(weekday_text, "~"), weekday_x, weekday_y,
                inactive_segment_color, date_font)
    screen.text(weekday_text, weekday_x, weekday_y, rgb565(0, 225, 240), date_font)
    screen.text(make_text_placeholder(time_text, "8"), time_x, time_y,
                inactive_segment_color, time_font)
    screen.text(time_text, time_x, time_y, rgb565(255, 255, 255), time_font)
    date_x = (WIDTH - text_width(date_text, date_font)) // 2
    date_y = time_y + time_font.height + date_gap
    screen.text(make_text_placeholder(date_text, "~"), date_x, date_y,
                inactive_segment_color, date_font)
    screen.text(date_text, date_x, date_y, rgb565(0, 225, 240), date_font)
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
