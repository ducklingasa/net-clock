#ifndef CLOCK_FONT_H
#define CLOCK_FONT_H

#include <stdint.h>

typedef struct {
    const char *characters;
    const uint8_t *alpha;
    uint16_t stride;
    uint8_t width;
    uint8_t height;
    uint8_t phases;
    uint8_t advance_half;
    uint8_t colon_adjust_half;
} clock_font_t;

extern const clock_font_t clock_font_small;
extern const clock_font_t clock_font_date;
extern const clock_font_t clock_font_humidity;
extern const clock_font_t clock_font_temperature;
extern const clock_font_t clock_font_time;
extern const clock_font_t clock_font_narrow_time;

const uint8_t *clock_font_glyph(const clock_font_t *font, uint32_t character,
                                unsigned phase);

#endif