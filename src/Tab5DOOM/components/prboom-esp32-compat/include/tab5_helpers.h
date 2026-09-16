#ifndef TAB5_HELPERS_H
#define TAB5_HELPERS_H

#include <stdbool.h>
#include <stdint.h>

#define TAB5_DOOM_WIDTH 320
#define TAB5_DOOM_HEIGHT 240
#define TAB5_DOOM_SCALE 3

typedef struct {
    uint16_t keys[5 * 14];
    uint16_t mask;
} tab5_key_state_t;

/* Track physical keys separately: several keys can hold the same action. */
static inline uint16_t tab5_key_update(tab5_key_state_t *state, unsigned row,
                                       unsigned col, bool pressed, uint16_t mask)
{
    if (row >= 5 || col >= 14) return state->mask;
    unsigned key = row * 14 + col;
    uint16_t value = pressed ? mask : 0;
    if (state->keys[key] == value) return state->mask;
    state->keys[key] = value;
    state->mask = 0;
    for (unsigned i = 0; i < 5 * 14; i++) state->mask |= state->keys[i];
    return state->mask;
}

/* Nearest-neighbor 3x scaling followed by a counter-clockwise 90deg rotation. */
static inline void tab5_scale_rgb565(const uint8_t *src, const uint16_t *palette,
                                     uint16_t *dst)
{
    const unsigned stride = TAB5_DOOM_HEIGHT * TAB5_DOOM_SCALE;
    for (int x = TAB5_DOOM_WIDTH - 1; x >= 0; x--) {
        uint16_t *row0 = dst;
        uint16_t *row1 = dst + stride;
        uint16_t *row2 = dst + 2 * stride;
        for (unsigned y = 0; y < TAB5_DOOM_HEIGHT; y++) {
            uint16_t color = palette[src[y * TAB5_DOOM_WIDTH + x]];
            /* Consecutive, aligned 16-bit writes keep PSRAM cache lines local. */
            row0[0] = row0[1] = row0[2] = color;
            row1[0] = row1[1] = row1[2] = color;
            row2[0] = row2[1] = row2[2] = color;
            row0 += 3;
            row1 += 3;
            row2 += 3;
        }
        dst += 3 * stride;
    }
}

/* A deterministic RGB565 test image independent of Doom and its palette. */
static inline void tab5_display_test_pattern(uint16_t *dst)
{
    static const uint16_t bars[8] = {
        0xffff, 0xffe0, 0x07ff, 0x07e0, 0xf81f, 0xf800, 0x001f, 0x8410
    };
    const unsigned width = TAB5_DOOM_HEIGHT * TAB5_DOOM_SCALE;
    const unsigned height = TAB5_DOOM_WIDTH * TAB5_DOOM_SCALE;
    for (unsigned y = 0; y < height; y++) {
        for (unsigned x = 0; x < width; x++) {
            uint16_t color = y < height / 2 ? bars[x * 8 / width] :
                (((x / 60) + (y / 60)) % 2 ? 0x4208 : 0xbdf7);
            if (x < 4 || y < 4 || x >= width - 4 || y >= height - 4) {
                color = 0xffff;
            }
            dst[y * width + x] = color;
        }
    }
}

/* Called only when due; unsigned subtraction also handles tick wraparound. */
static inline uint32_t tab5_next_frame_tick(uint32_t previous, uint32_t now,
                                            uint32_t interval)
{
    return now + interval - (now - previous) % interval;
}

#endif
