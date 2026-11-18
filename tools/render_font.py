#!/usr/bin/env python3
"""Bake original size-1 digits and rendered fonts into font.c."""

from __future__ import annotations

import argparse
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "main" / "font.c"
FONT_PATH = ROOT / "tools" / "fonts" / "JetBrainsMono-ExtraBold.ttf"
DSEG14_FONT_PATH = ROOT / "tools" / "fonts" / "DSEG14ClassicMini-Bold.ttf"
DSEG7_FONT_PATH = ROOT / "tools" / "fonts" / "DSEG7ClassicMini-Bold.ttf"
CHARACTERS = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ:-.%"
BITMAPS = (
    (0x3E, 0x51, 0x49, 0x45, 0x3E), (0x00, 0x42, 0x7F, 0x40, 0x00),
    (0x42, 0x61, 0x51, 0x49, 0x46), (0x21, 0x41, 0x45, 0x4B, 0x31),
    (0x18, 0x14, 0x12, 0x7F, 0x10), (0x27, 0x45, 0x45, 0x45, 0x39),
    (0x3C, 0x4A, 0x49, 0x49, 0x30), (0x01, 0x71, 0x09, 0x05, 0x03),
    (0x36, 0x49, 0x49, 0x49, 0x36), (0x06, 0x49, 0x49, 0x29, 0x1E),
    (0x7E, 0x11, 0x11, 0x11, 0x7E), (0x7F, 0x49, 0x49, 0x49, 0x36),
    (0x3E, 0x41, 0x41, 0x41, 0x22), (0x7F, 0x41, 0x41, 0x22, 0x1C),
    (0x7F, 0x49, 0x49, 0x49, 0x41), (0x7F, 0x09, 0x09, 0x09, 0x01),
    (0x3E, 0x41, 0x49, 0x49, 0x7A), (0x7F, 0x08, 0x08, 0x08, 0x7F),
    (0x00, 0x41, 0x7F, 0x41, 0x00), (0x20, 0x40, 0x41, 0x3F, 0x01),
    (0x7F, 0x08, 0x14, 0x22, 0x41), (0x7F, 0x40, 0x40, 0x40, 0x40),
    (0x7F, 0x02, 0x0C, 0x02, 0x7F), (0x7F, 0x04, 0x08, 0x10, 0x7F),
    (0x3E, 0x41, 0x41, 0x41, 0x3E), (0x7F, 0x09, 0x09, 0x09, 0x06),
    (0x3E, 0x41, 0x51, 0x21, 0x5E), (0x7F, 0x09, 0x19, 0x29, 0x46),
    (0x46, 0x49, 0x49, 0x49, 0x31), (0x01, 0x01, 0x7F, 0x01, 0x01),
    (0x3F, 0x40, 0x40, 0x40, 0x3F), (0x1F, 0x20, 0x40, 0x20, 0x1F),
    (0x3F, 0x40, 0x38, 0x40, 0x3F), (0x63, 0x14, 0x08, 0x14, 0x63),
    (0x07, 0x08, 0x70, 0x08, 0x07), (0x61, 0x51, 0x49, 0x45, 0x43),
    (0x00, 0x36, 0x36, 0x00, 0x00), (0x08, 0x08, 0x08, 0x08, 0x08),
    (0x00, 0x60, 0x60, 0x00, 0x00), (0x63, 0x13, 0x08, 0x64, 0x63),
)
SOURCE = dict(zip(CHARACTERS, BITMAPS))
DATE_LETTERS = "".join(sorted(set("MON TUE WED THU FRI SAT SUN "
                                     "JAN FEB MAR APR MAY JUN JUL AUG SEP OCT NOV DEC")))
SIZES = (
    ("small", 1, 1, 1, "0123456789", 0),
    ("date", 3, 3, 3, "0123456789%-~\u00b0" + DATE_LETTERS, 0),
    ("humidity", 3, 3, 3, "0123456789%-", 0),
    ("temperature", 4, 4, 4, "0123456789-\u00b0C", 0),
    ("time", 6, 6, 6, "0123456789: ", 24),
    ("narrow_time", 4.5, 6, 6, "0123456789: ", 18),
)


def rasterize(character: str, scale_x: float, scale_y: int, phase: int,
              font_path: Path = FONT_PATH) -> list[int]:
    width, height = math.ceil(5 * scale_x), 7 * scale_y
    glyph = SOURCE.get(character, (0, 0, 0, 0, 0))
    if scale_x == scale_y == 1:
        return [15 if glyph[column] & (1 << row) else 0
                for row in range(7) for column in range(5)]
    font = ImageFont.truetype(str(font_path), 256)
    if font_path == DSEG7_FONT_PATH:
        characters = set("0123456789:")
    elif font_path == DSEG14_FONT_PATH:
        characters = set(SIZES[1][4]) - {' '}
    else:
        characters = set(''.join(size[4] for size in SIZES[1:])) - {' '}
    bounds = [font.getbbox(letter, anchor='ls') for letter in characters]
    left = min(bound[0] for bound in bounds)
    top = min(bound[1] for bound in bounds)
    right = max(bound[2] for bound in bounds)
    bottom = max(bound[3] for bound in bounds)
    source = Image.new('L', (right - left, bottom - top))
    ImageDraw.Draw(source).text((-left, -top), character, font=font,
                               fill=255, anchor='ls')
    samples = 8
    scaled = source.resize((round(5 * scale_x * samples), height * samples),
                           Image.Resampling.LANCZOS)
    coverage = Image.new('L', (width * samples, height * samples))
    colon_offset = scale_x if character == ':' and font_path == DSEG7_FONT_PATH else 0
    coverage.paste(scaled, (round(colon_offset * samples) + phase * samples // 2, 0))
    coverage = coverage.resize((width, height), Image.Resampling.BOX)
    return [(value * 15 + 127) // 255 for value in coverage.tobytes()]


def c_string(value: str) -> str:
    return '"' + ''.join(
        character if ord(character) < 128 else ''.join(
            f'\\{byte:03o}' for byte in character.encode('utf-8'))
        for character in value) + '"'


def generate() -> str:
    output = ['#include "font.h"', '#include <stddef.h>', '#include <string.h>', '']
    descriptors = []
    total_bytes = 0
    for name, scale_x, scale_y, spacing, characters, colon_adjust_half in SIZES:
        width, height = math.ceil(5 * scale_x), 7 * scale_y
        phases = 2 if scale_x == 4.5 else 1
        if name == "date":
            font_path = DSEG14_FONT_PATH
        elif name in ("time", "narrow_time"):
            font_path = DSEG7_FONT_PATH
        else:
            font_path = FONT_PATH
        stride = (width * height + 1) // 2
        packed = []
        for character in characters:
            for phase in range(phases):
                pixels = rasterize(character, scale_x, scale_y, phase, font_path)
                assert len(pixels) == width * height
                assert all(0 <= value <= 15 for value in pixels)
                if character == ' ':
                    assert not any(pixels)
                elif scale_x != 1:
                    assert any(0 < value < 15 for value in pixels)
                pixels += [0] * (len(pixels) % 2)
                packed.extend((pixels[index] << 4) | pixels[index + 1]
                              for index in range(0, len(pixels), 2))
        output.append(f'static const uint8_t font_{name}_alpha[] = {{')
        for offset in range(0, len(packed), 16):
            output.append('    ' + ', '.join(f'0x{value:02X}' for value in packed[offset:offset + 16]) + ',')
        output.extend(['};', ''])
        descriptors.extend([f'const clock_font_t clock_font_{name} = {{',
                    f'    {c_string(characters)}, font_{name}_alpha, {stride}, '
                            f'{width}, {height}, {phases}, '
                            f'{int((5 * scale_x + spacing) * 2)}, {colon_adjust_half},',
                            '};', ''])
        total_bytes += len(packed)
    output.extend(descriptors)
    output.extend([
                   'const uint8_t *clock_font_glyph(const clock_font_t *font, uint32_t character, unsigned phase)',
                   '{', "    if (font == NULL || character == '\\0' || phase >= font->phases) {",
                   '        return NULL;', '    }',
                   '    const unsigned char *cursor = (const unsigned char *)font->characters;',
                   '    size_t glyph_index = 0;',
                   "    while (*cursor != '\\0') {",
                   '        uint32_t font_character = *cursor++;',
                   '        if ((font_character & 0xE0) == 0xC0 && (*cursor & 0xC0) == 0x80) {',
                   '            font_character = ((font_character & 0x1F) << 6) | (*cursor++ & 0x3F);',
                   '        }',
                   '        if (font_character == character) {',
                   '            return font->alpha + (glyph_index * font->phases + phase) * font->stride;',
                   '        }',
                   '        ++glyph_index;',
                   '    }',
                   '    return NULL;',
                   '}', ''])
    print(f'Font coverage: {total_bytes} bytes, {len(SIZES)} fonts; narrow time includes two subpixel phases')
    return '\n'.join(output)


def validate() -> None:
    assert ImageFont.truetype(str(FONT_PATH), 256).getname() == ('JetBrains Mono', 'ExtraBold')
    assert ImageFont.truetype(str(DSEG14_FONT_PATH), 256).getname() == ('DSEG14 Classic Mini', 'Bold')
    assert ImageFont.truetype(str(DSEG7_FONT_PATH), 256).getname() == ('DSEG7 Classic Mini', 'Bold')
    assert len(SOURCE) == len(BITMAPS) == len(CHARACTERS)
    date_characters = set(SIZES[1][4])
    assert set("0123456789-%~\u00b0 C") <= date_characters
    assert set("0123456789-\u00b0C") <= set(SIZES[3][4])
    for word in "MON TUE WED THU FRI SAT SUN JAN FEB MAR APR MAY JUN JUL AUG SEP OCT NOV DEC".split():
        assert set(word) <= date_characters
    for _, scale_x, _, spacing, _, _ in SIZES:
        for length in range(9):
            old_width = 0 if not length else math.floor((length - 1) * (5 * scale_x + spacing) + 5 * scale_x + 0.5)
            new_width = max(0, math.floor(length * (5 * scale_x + spacing) - spacing + 0.5))
            assert old_width == new_width
    for character in SIZES[0][4]:
        pixels = rasterize(character, 1, 1, 0)
        assert all(bool(pixels[row * 5 + column]) == bool(SOURCE[character][column] & (1 << row))
                   for row in range(7) for column in range(5))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true', help='Verify font.c matches its generator')
    parser.add_argument('--self-test', action='store_true', help='Validate without writing files')
    arguments = parser.parse_args()
    validate()
    generated = generate()
    if arguments.check:
        if OUTPUT.read_text(encoding='ascii') != generated:
            raise SystemExit('font.c is stale; run tools/render_font.py')
    elif not arguments.self_test:
        OUTPUT.write_text(generated, encoding='ascii')
    print('Font character coverage, metrics, and raster checks passed')


if __name__ == '__main__':
    main()