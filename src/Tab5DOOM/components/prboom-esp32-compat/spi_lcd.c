// Copyright 2016-2017 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "sdkconfig.h"

#if CONFIG_HW_M5STACK_TAB5

#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/ppa.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "bsp/m5stack_tab5.h"
#include "lvgl.h"
#include "tab5_helpers.h"

#define DOOM_W TAB5_DOOM_WIDTH
#define DOOM_H TAB5_DOOM_HEIGHT
#define TAB5_SCALE TAB5_DOOM_SCALE
#define TAB5_FB_W (DOOM_W * TAB5_SCALE)
#define TAB5_FB_H (DOOM_H * TAB5_SCALE)
#define TAB5_OUT_W TAB5_FB_H
#define TAB5_OUT_H TAB5_FB_W
#define TAB5_FRAME_INTERVAL_MS 50
#define TAB5_NATIVE_FB_SIZE (DOOM_W * DOOM_H * sizeof(uint16_t))
#define TAB5_SCALED_FB_SIZE (TAB5_OUT_W * TAB5_OUT_H * sizeof(uint16_t))
#define TAB5_FB_ALIGNMENT \
    ((CONFIG_CACHE_L1_CACHE_LINE_SIZE > CONFIG_CACHE_L2_CACHE_LINE_SIZE) ? \
     CONFIG_CACHE_L1_CACHE_LINE_SIZE : CONFIG_CACHE_L2_CACHE_LINE_SIZE)

static const char *TAG = "tab5_lcd";
static SemaphoreHandle_t frameMutex;
static lv_display_t *doom_disp;
static lv_obj_t *doom_img;
static lv_obj_t *diagnostic_label;
/* Updated and consumed by the Doom task, never by the LVGL task. */
static unsigned diagnostic_mode;
static bool diagnostic_pattern_pending;
static bool diagnostic_refresh_pending;
static const char *diagnostic_names[] = {
    "T: display test", "TEST 1: STATIC REDRAW", "TEST 2: STATIC HOLD"
};
static uint16_t *scaled_fb[3];
static uint32_t *native_fb;
static ppa_client_handle_t scale_ppa;
static bool ppa_scale_enabled;
static int scaled_fb_idx;
static TickType_t next_frame_tick;
static bool frame_deadline_valid;
static TickType_t last_stats_tick;
static bool stats_window_valid;
static uint32_t sent_frames;
static uint32_t skipped_frames;
static uint32_t processed_frames;
static uint32_t cpu_scaled_frames;
static uint32_t ppa_failures;
static uint64_t scale_time_total_us;
static uint64_t refresh_time_total_us;
static uint32_t scale_time_max_us;
static uint32_t refresh_time_max_us;

extern uint16_t lcdpal[256];

#if LVGL_VERSION_MAJOR >= 9
static lv_image_dsc_t doom_img_dsc[3];
#else
static lv_img_dsc_t doom_img_dsc[3];
#endif

static void tab5_lcd_init_img_dsc(int idx, const uint8_t *data)
{
#if LVGL_VERSION_MAJOR >= 9
    doom_img_dsc[idx].header.magic = LV_IMAGE_HEADER_MAGIC;
    doom_img_dsc[idx].header.cf = LV_COLOR_FORMAT_RGB565;
    doom_img_dsc[idx].header.w = TAB5_OUT_W;
    doom_img_dsc[idx].header.h = TAB5_OUT_H;
    doom_img_dsc[idx].header.stride = TAB5_OUT_W * 2;
    doom_img_dsc[idx].data_size = TAB5_SCALED_FB_SIZE;
    doom_img_dsc[idx].data = data;
#else
    doom_img_dsc[idx].header.always_zero = 0;
    doom_img_dsc[idx].header.cf = LV_IMG_CF_TRUE_COLOR;
    doom_img_dsc[idx].header.w = TAB5_OUT_W;
    doom_img_dsc[idx].header.h = TAB5_OUT_H;
    doom_img_dsc[idx].data_size = TAB5_SCALED_FB_SIZE;
    doom_img_dsc[idx].data = data;
#endif
}

static void tab5_lcd_log_stats(TickType_t now)
{
    if (!stats_window_valid) {
        last_stats_tick = now;
        stats_window_valid = true;
        return;
    }

    if ((uint32_t)(now - last_stats_tick) < pdMS_TO_TICKS(5000)) {
        return;
    }

    uint32_t window_ms = (uint32_t)(now - last_stats_tick) * portTICK_PERIOD_MS;
    uint32_t fps_x100 = (uint32_t)((uint64_t)sent_frames * 100000 / window_ms);
    ESP_LOGI(TAG,
             "frames sent=%" PRIu32 " skipped=%" PRIu32
             " window=%" PRIu32 "ms fps=%" PRIu32 ".%02" PRIu32
             " cpu_scale=%" PRIu32 " ppa_fail=%" PRIu32 " diag=%u"
             " scale=%" PRIu64 "/%" PRIu32 "us refresh=%" PRIu64 "/%" PRIu32
             "us free_int=%u free_psram=%u",
             sent_frames,
             skipped_frames,
             window_ms,
             fps_x100 / 100,
             fps_x100 % 100,
             cpu_scaled_frames,
             ppa_failures,
             diagnostic_mode,
             processed_frames ? scale_time_total_us / processed_frames : 0,
             scale_time_max_us,
             sent_frames ? refresh_time_total_us / sent_frames : 0,
             refresh_time_max_us,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    sent_frames = 0;
    skipped_frames = 0;
    processed_frames = 0;
    cpu_scaled_frames = 0;
    ppa_failures = 0;
    scale_time_total_us = 0;
    refresh_time_total_us = 0;
    scale_time_max_us = 0;
    refresh_time_max_us = 0;
    last_stats_tick = now;
}

static void tab5_lcd_scale_cpu(const uint8_t *scr, uint16_t *dst_fb)
{
    tab5_scale_rgb565(scr, lcdpal, dst_fb);
}

static bool tab5_lcd_scale_ppa(const uint8_t *scr, uint16_t *dst_fb)
{
    if (!ppa_scale_enabled) {
        return false;
    }

    for (size_t pixel = 0, word = 0; pixel < DOOM_W * DOOM_H; pixel += 2, word++) {
        uint32_t color0 = lcdpal[scr[pixel]];
        uint32_t color1 = lcdpal[scr[pixel + 1]];
        native_fb[word] = color0 | (color1 << 16);
    }

    const ppa_srm_oper_config_t config = {
        .in = {
            .buffer = native_fb,
            .pic_w = DOOM_W,
            .pic_h = DOOM_H,
            .block_w = DOOM_W,
            .block_h = DOOM_H,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = dst_fb,
            .buffer_size = TAB5_SCALED_FB_SIZE,
            .pic_w = TAB5_OUT_W,
            .pic_h = TAB5_OUT_H,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_90,
        .scale_x = TAB5_SCALE,
        .scale_y = TAB5_SCALE,
        .alpha_update_mode = PPA_ALPHA_NO_CHANGE,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    esp_err_t err = ppa_do_scale_rotate_mirror(scale_ppa, &config);

    if (err != ESP_OK) {
        ppa_scale_enabled = false;
        ppa_failures++;
        ESP_LOGW(TAG, "PPA scale failed (%s); using CPU fallback", esp_err_to_name(err));
        esp_err_t unregister_err = ppa_unregister_client(scale_ppa);
        if (unregister_err != ESP_OK) {
            ESP_LOGW(TAG, "PPA client cleanup failed (%s)",
                     esp_err_to_name(unregister_err));
        }
        scale_ppa = NULL;
        heap_caps_free(native_fb);
        native_fb = NULL;
        return false;
    }
    return true;
}

static void tab5_lcd_schedule_next_frame(TickType_t now)
{
    const TickType_t interval = pdMS_TO_TICKS(TAB5_FRAME_INTERVAL_MS);

    if (!frame_deadline_valid) {
        next_frame_tick = now + interval;
        frame_deadline_valid = true;
        return;
    }

    /*
     * Advance from the previous deadline instead of from "now". Doom presents
     * on a 35 Hz cadence, so resetting from now would quantize a 20 FPS target
     * down to 17.5 FPS (one frame every two game tics).
     */
    next_frame_tick = tab5_next_frame_tick(next_frame_tick, now, interval);
}

void spi_lcd_cycle_diagnostic(void)
{
    diagnostic_mode = (diagnostic_mode + 1) % 3;
    if (diagnostic_mode == 1) {
        diagnostic_pattern_pending = true;
    }
    diagnostic_refresh_pending = true;
    frame_deadline_valid = false;
    ESP_LOGI(TAG, "Display diagnostic: %s; T advances mode", diagnostic_names[diagnostic_mode]);
}

bool spi_lcd_frame_due(void)
{
    TickType_t now = xTaskGetTickCount();

    if (diagnostic_mode == 2 && !diagnostic_refresh_pending) {
        tab5_lcd_log_stats(now);
        return false;
    }

    if (frame_deadline_valid && (int32_t)(now - next_frame_tick) < 0) {
        skipped_frames++;
        tab5_lcd_log_stats(now);
        return false;
    }
    return true;
}

void spi_lcd_wait_finish()
{
    if (frameMutex) {
        xSemaphoreTake(frameMutex, portMAX_DELAY);
        xSemaphoreGive(frameMutex);
    }
}

void spi_lcd_send(const uint8_t *scr)
{
    TickType_t now = xTaskGetTickCount();

    if (!scaled_fb[0] || !scaled_fb[1] || !scaled_fb[2] || !doom_img) {
        return;
    }
    if (diagnostic_mode == 2 && !diagnostic_refresh_pending) {
        return;
    }

    if (frame_deadline_valid && (int32_t)(now - next_frame_tick) < 0) {
        skipped_frames++;
        tab5_lcd_log_stats(now);
        return;
    }
    tab5_lcd_schedule_next_frame(now);

    xSemaphoreTake(frameMutex, portMAX_DELAY);
    int next_fb_idx = diagnostic_mode && !diagnostic_pattern_pending ?
        scaled_fb_idx : (scaled_fb_idx + 1) % 3;
    uint16_t *dst_fb = scaled_fb[next_fb_idx];
    int64_t scale_start_us = esp_timer_get_time();

    if (diagnostic_mode) {
        if (diagnostic_pattern_pending) {
            tab5_display_test_pattern(dst_fb);
            diagnostic_pattern_pending = false;
        }
    } else if (!tab5_lcd_scale_ppa(scr, dst_fb)) {
        tab5_lcd_scale_cpu(scr, dst_fb);
        cpu_scaled_frames++;
    }
    uint32_t scale_time_us = (uint32_t)(esp_timer_get_time() - scale_start_us);
    processed_frames++;
    scale_time_total_us += scale_time_us;
    if (scale_time_us > scale_time_max_us) {
        scale_time_max_us = scale_time_us;
    }

    int displayed = 0;
    if (bsp_display_lock(0)) {
        int64_t refresh_start_us = esp_timer_get_time();
        scaled_fb_idx = next_fb_idx;
        if (diagnostic_refresh_pending) {
            lv_label_set_text(diagnostic_label, diagnostic_names[diagnostic_mode]);
        }
#if LVGL_VERSION_MAJOR >= 9
        lv_image_set_src(doom_img, &doom_img_dsc[scaled_fb_idx]);
        lv_obj_invalidate(doom_img);
#else
        lv_img_set_src(doom_img, &doom_img_dsc[scaled_fb_idx]);
        lv_obj_invalidate(doom_img);
#endif
        lv_refr_now(doom_disp);
        diagnostic_refresh_pending = false;
        uint32_t refresh_time_us = (uint32_t)(esp_timer_get_time() - refresh_start_us);
        refresh_time_total_us += refresh_time_us;
        if (refresh_time_us > refresh_time_max_us) {
            refresh_time_max_us = refresh_time_us;
        }
        bsp_display_unlock();
        displayed = 1;
    }

    xSemaphoreGive(frameMutex);
    if (displayed) {
        sent_frames++;
    } else {
        skipped_frames++;
    }
    tab5_lcd_log_stats(xTaskGetTickCount());
}

void spi_lcd_init()
{
    lv_display_t *disp;

    printf("tab5_lcd_init()\n");
    frameMutex = xSemaphoreCreateMutex();
    assert(frameMutex);

    ESP_ERROR_CHECK(bsp_i2c_init());
    bsp_io_expander_pi4ioe_init(bsp_i2c_get_handle());
    bsp_reset_tp();
    vTaskDelay(pdMS_TO_TICKS(50));

    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
#if CONFIG_BSP_DISPLAY_LVGL_AVOID_TEAR
        /* The LVGL port obtains both full-size buffers from the DPI driver.
         * It draws into the back buffer and waits for scanout handoff instead
         * of copying 20-line chunks into the currently visible frame. */
        .buffer_size = BSP_LCD_H_RES * BSP_LCD_V_RES,
        .double_buffer = true,
#else
        .buffer_size = BSP_LCD_H_RES * 20,
        .double_buffer = false,
#endif
        .flags = {
#if CONFIG_BSP_LCD_COLOR_FORMAT_RGB888
            .buff_dma = false,
#else
            .buff_dma = true,
#endif
            .buff_spiram = false,
            .sw_rotate = false,
        },
    };

    disp = bsp_display_start_with_config(&cfg);
    assert(disp);
    doom_disp = disp;
    bsp_display_brightness_set(100);
    bsp_display_backlight_on();

    for (int i = 0; i < 3; i++) {
        scaled_fb[i] = heap_caps_aligned_calloc(TAB5_FB_ALIGNMENT,
                                               1,
                                               TAB5_SCALED_FB_SIZE,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        assert(scaled_fb[i]);
        tab5_lcd_init_img_dsc(i, (const uint8_t *)scaled_fb[i]);
    }
    scaled_fb_idx = 0;

#if CONFIG_HW_TAB5_PPA_ENA
    native_fb = heap_caps_aligned_calloc(TAB5_FB_ALIGNMENT,
                                         1,
                                         TAB5_NATIVE_FB_SIZE,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (native_fb) {
        const ppa_client_config_t ppa_config = {
            .oper_type = PPA_OPERATION_SRM,
            .max_pending_trans_num = 1,
            /* LCD scanout shares PSRAM with PPA. Shorter bursts leave time
             * for scanout instead of maximizing PPA's peak bandwidth. */
            .data_burst_length = PPA_DATA_BURST_LENGTH_32,
        };
        esp_err_t err = ppa_register_client(&ppa_config, &scale_ppa);
        if (err == ESP_OK) {
            ppa_scale_enabled = true;
        } else {
            ESP_LOGW(TAG, "PPA client registration failed (%s); using CPU scaler",
                     esp_err_to_name(err));
            heap_caps_free(native_fb);
            native_fb = NULL;
        }
    } else {
        ESP_LOGW(TAG, "PPA input buffer allocation failed; using CPU scaler");
    }
#else
    ESP_LOGI(TAG, "PPA disabled by configuration; using CPU scaler");
#endif

    if (bsp_display_lock(0)) {
        lv_obj_t *screen = lv_screen_active();
        lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
#if LVGL_VERSION_MAJOR >= 9
        doom_img = lv_image_create(screen);
        lv_image_set_src(doom_img, &doom_img_dsc[scaled_fb_idx]);
#else
        doom_img = lv_img_create(screen);
        lv_img_set_src(doom_img, &doom_img_dsc[scaled_fb_idx]);
#endif
        lv_obj_align(doom_img, LV_ALIGN_CENTER, 0, 0);
        diagnostic_label = lv_label_create(screen);
        lv_label_set_text(diagnostic_label, diagnostic_names[0]);
        lv_obj_set_style_text_color(diagnostic_label, lv_color_white(), LV_PART_MAIN);
        lv_obj_align(diagnostic_label, LV_ALIGN_BOTTOM_MID, 0, -12);
        bsp_display_unlock();
    }

    ESP_LOGI(TAG,
             "Tab5 display ready: Doom %dx%d -> %dx%d (3x + 90deg), draw_buf=%u px, max_fps=%u, scaler=%s",
             DOOM_W,
             DOOM_H,
             TAB5_OUT_W,
             TAB5_OUT_H,
             (unsigned)cfg.buffer_size,
             (unsigned)(1000 / TAB5_FRAME_INTERVAL_MS),
             ppa_scale_enabled ? "PPA" : "CPU");
    ESP_LOGI(TAG, "Display bandwidth config: PSRAM=%uMHz L2=%uKB PPA_burst=%uB",
             (unsigned)CONFIG_SPIRAM_SPEED,
             (unsigned)(CONFIG_CACHE_L2_CACHE_SIZE / 1024),
             ppa_scale_enabled ? 32u : 0u);
#if CONFIG_BSP_DISPLAY_LVGL_AVOID_TEAR
    ESP_LOGI(TAG, "Presentation: LCD buffers=%u, frame-boundary handoff enabled",
             (unsigned)CONFIG_BSP_LCD_DPI_BUFFER_NUMS);
#else
    ESP_LOGI(TAG, "Presentation: partial updates to LCD scan buffer");
#endif
}

#else

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "driver/spi_master.h"
#include "soc/gpio_struct.h"
#include "driver/gpio.h"
#include "esp_heap_alloc_caps.h"

#if 0
#define PIN_NUM_MISO 25
#define PIN_NUM_MOSI 23
#define PIN_NUM_CLK  19
#define PIN_NUM_CS   22
#define PIN_NUM_DC   21
#define PIN_NUM_RST  18
#define PIN_NUM_BCKL 5
#else
#define PIN_NUM_MOSI CONFIG_HW_LCD_MOSI_GPIO
#define PIN_NUM_CLK  CONFIG_HW_LCD_CLK_GPIO
#define PIN_NUM_CS   CONFIG_HW_LCD_CS_GPIO
#define PIN_NUM_DC   CONFIG_HW_LCD_DC_GPIO
#define PIN_NUM_RST  CONFIG_HW_LCD_RESET_GPIO
#define PIN_NUM_BCKL CONFIG_HW_LCD_BL_GPIO
#endif

//You want this, especially at higher framerates. The 2nd buffer is allocated in iram anyway, so isn't really in the way.
#define DOUBLE_BUFFER


/*
 The LCD needs a bunch of command/argument values to be initialized. They are stored in this struct.
*/
typedef struct {
    uint8_t cmd;
    uint8_t data[16];
    uint8_t databytes; //No of data in data; bit 7 = delay after set; 0xFF = end of cmds.
} ili_init_cmd_t;


#if (CONFIG_HW_LCD_TYPE == 1)

static const ili_init_cmd_t ili_init_cmds[]={
    {0x36, {(1<<5)|(1<<6)}, 1},
    {0x3A, {0x55}, 1},
    {0xB2, {0x0c, 0x0c, 0x00, 0x33, 0x33}, 5},
    {0xB7, {0x45}, 1},
    {0xBB, {0x2B}, 1},
    {0xC0, {0x2C}, 1},
    {0xC2, {0x01, 0xff}, 2},
    {0xC3, {0x11}, 1},
    {0xC4, {0x20}, 1},
    {0xC6, {0x0f}, 1},
    {0xD0, {0xA4, 0xA1}, 1},
    {0xE0, {0xD0, 0x00, 0x05, 0x0E, 0x15, 0x0D, 0x37, 0x43, 0x47, 0x09, 0x15, 0x12, 0x16, 0x19}, 14},
    {0xE1, {0xD0, 0x00, 0x05, 0x0D, 0x0C, 0x06, 0x2D, 0x44, 0x40, 0x0E, 0x1C, 0x18, 0x16, 0x19}, 14},
    {0x11, {0}, 0x80},
    {0x29, {0}, 0x80},
    {0, {0}, 0xff}
};

#endif

#if (CONFIG_HW_LCD_TYPE == 0)


static const ili_init_cmd_t ili_init_cmds[]={
    {0xCF, {0x00, 0x83, 0X30}, 3},
    {0xED, {0x64, 0x03, 0X12, 0X81}, 4},
    {0xE8, {0x85, 0x01, 0x79}, 3},
    {0xCB, {0x39, 0x2C, 0x00, 0x34, 0x02}, 5},
    {0xF7, {0x20}, 1},
    {0xEA, {0x00, 0x00}, 2},
    {0xC0, {0x26}, 1},
    {0xC1, {0x11}, 1},
    {0xC5, {0x35, 0x3E}, 2},
    {0xC7, {0xBE}, 1},
    {0x36, {0x28}, 1},
    {0x3A, {0x55}, 1},
    {0xB1, {0x00, 0x1B}, 2},
    {0xF2, {0x08}, 1},
    {0x26, {0x01}, 1},
    {0xE0, {0x1F, 0x1A, 0x18, 0x0A, 0x0F, 0x06, 0x45, 0X87, 0x32, 0x0A, 0x07, 0x02, 0x07, 0x05, 0x00}, 15},
    {0XE1, {0x00, 0x25, 0x27, 0x05, 0x10, 0x09, 0x3A, 0x78, 0x4D, 0x05, 0x18, 0x0D, 0x38, 0x3A, 0x1F}, 15},
    {0x2A, {0x00, 0x00, 0x00, 0xEF}, 4},
    {0x2B, {0x00, 0x00, 0x01, 0x3f}, 4}, 
    {0x2C, {0}, 0},
    {0xB7, {0x07}, 1},
    {0xB6, {0x0A, 0x82, 0x27, 0x00}, 4},
    {0x11, {0}, 0x80},
    {0x29, {0}, 0x80},
    {0, {0}, 0xff},
};

#endif

static spi_device_handle_t spi;


//Send a command to the ILI9341. Uses spi_device_transmit, which waits until the transfer is complete.
void ili_cmd(spi_device_handle_t spi, const uint8_t cmd) 
{
    esp_err_t ret;
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));       //Zero out the transaction
    t.length=8;                     //Command is 8 bits
    t.tx_buffer=&cmd;               //The data is the cmd itself
    t.user=(void*)0;                //D/C needs to be set to 0
    ret=spi_device_transmit(spi, &t);  //Transmit!
    assert(ret==ESP_OK);            //Should have had no issues.
}

//Send data to the ILI9341. Uses spi_device_transmit, which waits until the transfer is complete.
void ili_data(spi_device_handle_t spi, const uint8_t *data, int len) 
{
    esp_err_t ret;
    spi_transaction_t t;
    if (len==0) return;             //no need to send anything
    memset(&t, 0, sizeof(t));       //Zero out the transaction
    t.length=len*8;                 //Len is in bytes, transaction length is in bits.
    t.tx_buffer=data;               //Data
    t.user=(void*)1;                //D/C needs to be set to 1
    ret=spi_device_transmit(spi, &t);  //Transmit!
    assert(ret==ESP_OK);            //Should have had no issues.
}

//This function is called (in irq context!) just before a transmission starts. It will
//set the D/C line to the value indicated in the user field.
void ili_spi_pre_transfer_callback(spi_transaction_t *t) 
{
    int dc=(int)t->user;
    gpio_set_level(PIN_NUM_DC, dc);
}

//Initialize the display
void ili_init(spi_device_handle_t spi) 
{
    int cmd=0;
    //Initialize non-SPI GPIOs
    gpio_set_direction(PIN_NUM_DC, GPIO_MODE_OUTPUT);
    gpio_set_direction(PIN_NUM_RST, GPIO_MODE_OUTPUT);
    gpio_set_direction(PIN_NUM_BCKL, GPIO_MODE_OUTPUT);

    //Reset the display
    gpio_set_level(PIN_NUM_RST, 0);
    vTaskDelay(100 / portTICK_RATE_MS);
    gpio_set_level(PIN_NUM_RST, 1);
    vTaskDelay(100 / portTICK_RATE_MS);

    //Send all the commands
    while (ili_init_cmds[cmd].databytes!=0xff) {
        uint8_t dmdata[16];
        ili_cmd(spi, ili_init_cmds[cmd].cmd);
        //Need to copy from flash to DMA'able memory
        memcpy(dmdata, ili_init_cmds[cmd].data, 16);
        ili_data(spi, dmdata, ili_init_cmds[cmd].databytes&0x1F);
        if (ili_init_cmds[cmd].databytes&0x80) {
            vTaskDelay(100 / portTICK_RATE_MS);
        }
        cmd++;
    }

    ///Enable backlight
#if CONFIG_HW_INV_BL
    gpio_set_level(PIN_NUM_BCKL, 0);
#else
    gpio_set_level(PIN_NUM_BCKL, 1);
#endif

}


static void send_header_start(spi_device_handle_t spi, int xpos, int ypos, int w, int h)
{
    esp_err_t ret;
    int x;
    //Transaction descriptors. Declared static so they're not allocated on the stack; we need this memory even when this
    //function is finished because the SPI driver needs access to it even while we're already calculating the next line.
    static spi_transaction_t trans[5];

    //In theory, it's better to initialize trans and data only once and hang on to the initialized
    //variables. We allocate them on the stack, so we need to re-init them each call.
    for (x=0; x<5; x++) {
        memset(&trans[x], 0, sizeof(spi_transaction_t));
        if ((x&1)==0) {
            //Even transfers are commands
            trans[x].length=8;
            trans[x].user=(void*)0;
        } else {
            //Odd transfers are data
            trans[x].length=8*4;
            trans[x].user=(void*)1;
        }
        trans[x].flags=SPI_TRANS_USE_TXDATA;
    }
    trans[0].tx_data[0]=0x2A;           //Column Address Set
    trans[1].tx_data[0]=xpos>>8;              //Start Col High
    trans[1].tx_data[1]=xpos;              //Start Col Low
    trans[1].tx_data[2]=(xpos+w-1)>>8;       //End Col High
    trans[1].tx_data[3]=(xpos+w-1)&0xff;     //End Col Low
    trans[2].tx_data[0]=0x2B;           //Page address set
    trans[3].tx_data[0]=ypos>>8;        //Start page high
    trans[3].tx_data[1]=ypos&0xff;      //start page low
    trans[3].tx_data[2]=(ypos+h-1)>>8;    //end page high
    trans[3].tx_data[3]=(ypos+h-1)&0xff;  //end page low
    trans[4].tx_data[0]=0x2C;           //memory write

    //Queue all transactions.
    for (x=0; x<5; x++) {
        ret=spi_device_queue_trans(spi, &trans[x], portMAX_DELAY);
        assert(ret==ESP_OK);
    }

    //When we are here, the SPI driver is busy (in the background) getting the transactions sent. That happens
    //mostly using DMA, so the CPU doesn't have much to do here. We're not going to wait for the transaction to
    //finish because we may as well spend the time calculating the next line. When that is done, we can call
    //send_line_finish, which will wait for the transfers to be done and check their status.
}


void send_header_cleanup(spi_device_handle_t spi) 
{
    spi_transaction_t *rtrans;
    esp_err_t ret;
    //Wait for all 5 transactions to be done and get back the results.
    for (int x=0; x<5; x++) {
        ret=spi_device_get_trans_result(spi, &rtrans, portMAX_DELAY);
        assert(ret==ESP_OK);
        //We could inspect rtrans now if we received any info back. The LCD is treated as write-only, though.
    }
}


#ifndef DOUBLE_BUFFER
volatile static uint16_t *currFbPtr=NULL;
#else
//Warning: This gets squeezed into IRAM.
static uint32_t *currFbPtr=NULL;
#endif
SemaphoreHandle_t dispSem = NULL;
SemaphoreHandle_t dispDoneSem = NULL;

#define NO_SIM_TRANS 5 //Amount of SPI transfers to queue in parallel
#define MEM_PER_TRANS 1024*3 //in 16-bit words

extern uint16_t lcdpal[256];

bool spi_lcd_frame_due(void)
{
    return true;
}

void IRAM_ATTR displayTask(void *arg) {
	int x, i;
	int idx=0;
	int inProgress=0;
	static uint16_t *dmamem[NO_SIM_TRANS];
	spi_transaction_t trans[NO_SIM_TRANS];
	spi_transaction_t *rtrans;

    esp_err_t ret;
    spi_bus_config_t buscfg={
        .miso_io_num=-1,
        .mosi_io_num=PIN_NUM_MOSI,
        .sclk_io_num=PIN_NUM_CLK,
        .quadwp_io_num=-1,
        .quadhd_io_num=-1,
        .max_transfer_sz=(MEM_PER_TRANS*2)+16
    };
    spi_device_interface_config_t devcfg={
        .clock_speed_hz=26000000,               //Clock out at 26 MHz. Yes, that's heavily overclocked.
        .mode=0,                                //SPI mode 0
        .spics_io_num=PIN_NUM_CS,               //CS pin
        .queue_size=NO_SIM_TRANS,               //We want to be able to queue this many transfers
        .pre_cb=ili_spi_pre_transfer_callback,  //Specify pre-transfer callback to handle D/C line
    };

	printf("*** Display task starting.\n");

    //Initialize the SPI bus
    ret=spi_bus_initialize(HSPI_HOST, &buscfg, 1);
    assert(ret==ESP_OK);
    //Attach the LCD to the SPI bus
    ret=spi_bus_add_device(HSPI_HOST, &devcfg, &spi);
    assert(ret==ESP_OK);
    //Initialize the LCD
    ili_init(spi);

	//We're going to do a fair few transfers in parallel. Set them all up.
	for (x=0; x<NO_SIM_TRANS; x++) {
		dmamem[x]=pvPortMallocCaps(MEM_PER_TRANS*2, MALLOC_CAP_DMA);
		assert(dmamem[x]);
		memset(&trans[x], 0, sizeof(spi_transaction_t));
		trans[x].length=MEM_PER_TRANS*2;
		trans[x].user=(void*)1;
		trans[x].tx_buffer=&dmamem[x];
	}
	xSemaphoreGive(dispDoneSem);

	while(1) {
		xSemaphoreTake(dispSem, portMAX_DELAY);
//		printf("Display task: frame.\n");
#ifndef DOUBLE_BUFFER
		uint8_t *myData=(uint8_t*)currFbPtr;
#endif

		send_header_start(spi, 0, 0, 320, 240);
		send_header_cleanup(spi);
		for (x=0; x<320*240; x+=MEM_PER_TRANS) {
#ifdef DOUBLE_BUFFER
			for (i=0; i<MEM_PER_TRANS; i+=4) {
				uint32_t d=currFbPtr[(x+i)/4];
				dmamem[idx][i+0]=lcdpal[(d>>0)&0xff];
				dmamem[idx][i+1]=lcdpal[(d>>8)&0xff];
				dmamem[idx][i+2]=lcdpal[(d>>16)&0xff];
				dmamem[idx][i+3]=lcdpal[(d>>24)&0xff];
			}
#else
			for (i=0; i<MEM_PER_TRANS; i++) {
				dmamem[idx][i]=lcdpal[myData[i]];
			}
			myData+=MEM_PER_TRANS;
#endif
			trans[idx].length=MEM_PER_TRANS*16;
			trans[idx].user=(void*)1;
			trans[idx].tx_buffer=dmamem[idx];
			ret=spi_device_queue_trans(spi, &trans[idx], portMAX_DELAY);
			assert(ret==ESP_OK);

			idx++;
			if (idx>=NO_SIM_TRANS) idx=0;

			if (inProgress==NO_SIM_TRANS-1) {
				ret=spi_device_get_trans_result(spi, &rtrans, portMAX_DELAY);
				assert(ret==ESP_OK);
			} else {
				inProgress++;
			}
		}
#ifndef DOUBLE_BUFFER
		xSemaphoreGive(dispDoneSem);
#endif
		while(inProgress) {
			ret=spi_device_get_trans_result(spi, &rtrans, portMAX_DELAY);
			assert(ret==ESP_OK);
			inProgress--;
		}
	}
}

#include    <xtensa/config/core.h>
#include    <xtensa/corebits.h>
#include    <xtensa/config/system.h>
#include    <xtensa/simcall.h>

void spi_lcd_wait_finish() {
#ifndef DOUBLE_BUFFER
	xSemaphoreTake(dispDoneSem, portMAX_DELAY);
#endif
}

void spi_lcd_send(const uint8_t *scr) {
#ifdef DOUBLE_BUFFER
	memcpy(currFbPtr, scr, 320*240);
	//Theoretically, also should double-buffer the lcdpal array... ahwell.
#else
	currFbPtr=(uint16_t *)scr;
#endif
	xSemaphoreGive(dispSem);
}

void spi_lcd_init() {
	printf("spi_lcd_init()\n");
    dispSem=xSemaphoreCreateBinary();
    dispDoneSem=xSemaphoreCreateBinary();
#ifdef DOUBLE_BUFFER
	currFbPtr=pvPortMallocCaps(320*240, MALLOC_CAP_32BIT);
#endif
#if CONFIG_FREERTOS_UNICORE
	xTaskCreatePinnedToCore(&displayTask, "display", 6000, NULL, 6, NULL, 0);
#else
	xTaskCreatePinnedToCore(&displayTask, "display", 6000, NULL, 6, NULL, 1);
#endif
}
#endif
