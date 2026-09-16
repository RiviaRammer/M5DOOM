#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tab5_helpers.h"

static uint32_t random_state = 0x12345678;

static uint32_t next_random(void)
{
    random_state = random_state * 1664525u + 1013904223u;
    return random_state;
}

static void test_keys(void)
{
    tab5_key_state_t state = {0};
    const uint16_t up = 1, fire = 16;
    assert(tab5_key_update(&state, 2, 2, true, up) == up);
    assert(tab5_key_update(&state, 3, 11, true, up) == up);
    assert(tab5_key_update(&state, 2, 2, false, up) == up);
    assert(tab5_key_update(&state, 2, 2, false, up) == up);
    assert(tab5_key_update(&state, 3, 11, true, up) == up);
    assert(tab5_key_update(&state, 4, 0, true, fire) == (up | fire));
    assert(tab5_key_update(&state, 3, 11, false, up) == fire);
    assert(tab5_key_update(&state, 5, 0, true, up) == fire);
    assert(tab5_key_update(&state, 0, 14, false, fire) == fire);
    assert(tab5_key_update(&state, 4, 0, false, fire) == 0);

    bool held[70] = {false};
    for (unsigned n = 0; n < 10000; n++) {
        unsigned key = next_random() % 70;
        bool pressed = (next_random() & 0x100) != 0;
        held[key] = pressed;
        uint16_t expected = 0;
        for (unsigned i = 0; i < 70; i++) {
            if (held[i]) expected |= (uint16_t)(1u << (i % 13));
        }
        assert(tab5_key_update(&state, key / 14, key % 14, pressed,
                              (uint16_t)(1u << (key % 13))) == expected);
    }
}

static void test_scale(void)
{
    const unsigned width = TAB5_DOOM_WIDTH;
    const unsigned height = TAB5_DOOM_HEIGHT;
    const unsigned scale = TAB5_DOOM_SCALE;
    const unsigned count = width * height * scale * scale;
    uint8_t *src = malloc(width * height);
    uint16_t *allocation = malloc((count + 2) * sizeof(*allocation));
    assert(src && allocation);
    uint16_t *dst = allocation + 1; /* Deliberately only 16-bit aligned. */
    uint16_t palette[256];
    for (unsigned i = 0; i < 256; i++) palette[i] = (uint16_t)(i * 257u);
    for (unsigned pass = 0; pass < 3; pass++) {
        for (unsigned i = 0; i < width * height; i++) {
            src[i] = pass == 0 ? (uint8_t)i :
                     pass == 1 ? (uint8_t)next_random() : 0xff;
        }
        allocation[0] = 0x1234;
        allocation[count + 1] = 0xabcd;
        memset(dst, 0xa5, count * sizeof(*dst));
        tab5_scale_rgb565(src, palette, dst);
        /* Independent inverse-coordinate oracle, checking every output pixel. */
        for (unsigned y = 0; y < width * scale; y++) {
            for (unsigned x = 0; x < height * scale; x++) {
                unsigned source_x = width - 1 - y / scale;
                unsigned source_y = x / scale;
                assert(dst[y * height * scale + x] ==
                       palette[src[source_y * width + source_x]]);
            }
        }
        assert(allocation[0] == 0x1234 && allocation[count + 1] == 0xabcd);
    }
    free(allocation);
    free(src);
}

static void test_display_pattern(void)
{
    const unsigned width = TAB5_DOOM_HEIGHT * TAB5_DOOM_SCALE;
    const unsigned height = TAB5_DOOM_WIDTH * TAB5_DOOM_SCALE;
    const unsigned count = width * height;
    uint16_t *allocation = malloc((count + 2) * sizeof(*allocation));
    assert(allocation);
    allocation[0] = 0x1234;
    allocation[count + 1] = 0xabcd;
    uint16_t *dst = allocation + 1;
    tab5_display_test_pattern(dst);
    const uint16_t expected_bars[] = {
        0xffff, 0xffe0, 0x07ff, 0x07e0, 0xf81f, 0xf800, 0x001f, 0x8410
    };
    for (unsigned y = 0; y < height; y++) {
        for (unsigned x = 0; x < width; x++) {
            uint16_t expected;
            if (x < 4 || x + 4 >= width || y < 4 || y + 4 >= height) {
                expected = 0xffff;
            } else if (y < height / 2) {
                expected = expected_bars[x / (width / 8)];
            } else {
                expected = ((x / 60) % 2 != (y / 60) % 2) ? 0x4208 : 0xbdf7;
            }
            assert(dst[y * width + x] == expected);
        }
    }
    assert(allocation[0] == 0x1234 && allocation[count + 1] == 0xabcd);
    free(allocation);
}

static void test_deadlines(void)
{
    for (uint32_t interval = 1; interval <= 20; interval++) {
        for (uint32_t elapsed = 0; elapsed < 1000; elapsed++) {
            uint32_t previous = UINT32_MAX - 100;
            uint32_t now = previous + elapsed;
            uint32_t expected = previous;
            do { expected += interval; } while ((int32_t)(now - expected) >= 0);
            assert(tab5_next_frame_tick(previous, now, interval) == expected);
        }
    }
    assert(tab5_next_frame_tick(UINT32_MAX - 4, UINT32_MAX - 4, 5) == 0);
    assert(tab5_next_frame_tick(0, 0, 5) == 5);
    assert(tab5_next_frame_tick(10, 1000000010, 5) == 1000000015);

    /* 35 Hz simulation at 100 Hz RTOS ticks must retain the 20 Hz frame grid. */
    uint32_t deadline = 0;
    unsigned frames = 0;
    for (unsigned tic = 0; tic < 350; tic++) {
        uint32_t now = tic * 100 / 35;
        if ((int32_t)(now - deadline) >= 0) {
            frames++;
            deadline = tab5_next_frame_tick(deadline, now, 5);
        }
    }
    assert(frames == 200);
}

int main(void)
{
    test_keys();
    test_scale();
    test_display_pattern();
    test_deadlines();
    puts("PASS: physical-key aggregation, RGB565 scaling, display pattern, frame deadlines");
    return 0;
}
