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


#include <stdlib.h>

#include "sdkconfig.h"

#include "doomdef.h"
#include "doomtype.h"
#include "m_argv.h"
#include "d_event.h"
#include "g_game.h"
#include "d_main.h"
#include "gamepad.h"
#include "lprintf.h"

#if CONFIG_HW_M5STACK_TAB5
#include "bsp/m5stack_tab5.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#else
#include "psxcontroller.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif


//The gamepad uses keyboard emulation, but for compilation, these variables need to be placed
//somewhere. THis is as good a place as any.
int usejoystick=0;
int joyleft, joyright, joyup, joydown;


//atomic, for communication between joy thread and main game thread
volatile int joyVal=0;

typedef struct {
	int mask;
	int *key;
} JsKeyMap;

#if CONFIG_HW_M5STACK_TAB5

enum {
	TAB5_BTN_UP          = 1 << 0,
	TAB5_BTN_DOWN        = 1 << 1,
	TAB5_BTN_LEFT        = 1 << 2,
	TAB5_BTN_RIGHT       = 1 << 3,
	TAB5_BTN_FIRE        = 1 << 4,
	TAB5_BTN_USE         = 1 << 5,
	TAB5_BTN_STRAFE      = 1 << 6,
	TAB5_BTN_SPEED       = 1 << 7,
	TAB5_BTN_MAP         = 1 << 8,
	TAB5_BTN_ESCAPE      = 1 << 9,
	TAB5_BTN_PAUSE       = 1 << 10,
	TAB5_BTN_WEAPON      = 1 << 11,
};

static const char *TAG = "tab5_input";
static bool tab5_kb_bus_ready;
static bool tab5_kb_ready;
static bool tab5_kb_hid_mode;
static TickType_t tab5_kb_next_probe_tick;
static TickType_t tab5_kb_next_poll_tick;
static TickType_t tab5_kb_next_error_log_tick;

#define TAB5_KEYBOARD_ADDR      0x6d
#define TAB5_KEYBOARD_SDA       GPIO_NUM_0
#define TAB5_KEYBOARD_SCL       GPIO_NUM_1
#define TAB5_KEYBOARD_INT       GPIO_NUM_50
#define TAB5_KEYBOARD_REG_INT_CFG   0x00
#define TAB5_KEYBOARD_REG_INT_STAT  0x01
#define TAB5_KEYBOARD_REG_EVENT_NUM 0x02
#define TAB5_KEYBOARD_REG_MODE      0x10
#define TAB5_KEYBOARD_REG_KEY_EVENT 0x20
#define TAB5_KEYBOARD_REG_HID_EVENT 0x30
#define TAB5_KEYBOARD_REG_VERSION   0xfe

static uint8_t tab5_kb_addr = TAB5_KEYBOARD_ADDR;
static int tab5KeyboardHidToMask(uint8_t modifier, uint8_t keycode);

static void tab5KeyboardDeinit(void)
{
	tab5_kb_ready = false;
	tab5_kb_hid_mode = false;
	tab5_kb_addr = TAB5_KEYBOARD_ADDR;
}

static void tab5KeyboardI2CDelay(void)
{
	esp_rom_delay_us(20);
}

static void tab5KeyboardSda(bool high)
{
	gpio_set_level(TAB5_KEYBOARD_SDA, high ? 1 : 0);
}

static void tab5KeyboardScl(bool high)
{
	gpio_set_level(TAB5_KEYBOARD_SCL, high ? 1 : 0);
}

static bool tab5KeyboardReadSda(void)
{
	return gpio_get_level(TAB5_KEYBOARD_SDA) != 0;
}

static void tab5KeyboardI2CStart(void)
{
	tab5KeyboardSda(true);
	tab5KeyboardScl(true);
	tab5KeyboardI2CDelay();
	tab5KeyboardSda(false);
	tab5KeyboardI2CDelay();
	tab5KeyboardScl(false);
	tab5KeyboardI2CDelay();
}

static void tab5KeyboardI2CStop(void)
{
	tab5KeyboardSda(false);
	tab5KeyboardI2CDelay();
	tab5KeyboardScl(true);
	tab5KeyboardI2CDelay();
	tab5KeyboardSda(true);
	tab5KeyboardI2CDelay();
}

static bool tab5KeyboardI2CWriteByte(uint8_t value)
{
	for (int bit = 7; bit >= 0; bit--) {
		tab5KeyboardSda((value & (1 << bit)) != 0);
		tab5KeyboardI2CDelay();
		tab5KeyboardScl(true);
		tab5KeyboardI2CDelay();
		tab5KeyboardScl(false);
		tab5KeyboardI2CDelay();
	}

	tab5KeyboardSda(true);
	tab5KeyboardI2CDelay();
	tab5KeyboardScl(true);
	tab5KeyboardI2CDelay();
	bool ack = !tab5KeyboardReadSda();
	tab5KeyboardScl(false);
	tab5KeyboardI2CDelay();
	return ack;
}

static uint8_t tab5KeyboardI2CReadByte(bool ack)
{
	uint8_t value = 0;

	tab5KeyboardSda(true);
	for (int bit = 7; bit >= 0; bit--) {
		tab5KeyboardI2CDelay();
		tab5KeyboardScl(true);
		tab5KeyboardI2CDelay();
		if (tab5KeyboardReadSda()) {
			value |= (1 << bit);
		}
		tab5KeyboardScl(false);
		tab5KeyboardI2CDelay();
	}

	tab5KeyboardSda(!ack);
	tab5KeyboardI2CDelay();
	tab5KeyboardScl(true);
	tab5KeyboardI2CDelay();
	tab5KeyboardScl(false);
	tab5KeyboardSda(true);
	tab5KeyboardI2CDelay();
	return value;
}

static bool tab5KeyboardI2CProbe(uint8_t addr)
{
	tab5KeyboardI2CStart();
	bool ack = tab5KeyboardI2CWriteByte((addr << 1) | 0);
	tab5KeyboardI2CStop();
	return ack;
}

static esp_err_t tab5KeyboardReadReg(uint8_t reg, uint8_t *data, size_t len)
{
	if (!data || !len) {
		return ESP_ERR_INVALID_ARG;
	}
	tab5KeyboardI2CStart();
	if (!tab5KeyboardI2CWriteByte((tab5_kb_addr << 1) | 0)) {
		tab5KeyboardI2CStop();
		ESP_LOGW(TAG, "Tab5 Keyboard read reg 0x%02x: write address NACK", reg);
		return ESP_ERR_NOT_FOUND;
	}
	if (!tab5KeyboardI2CWriteByte(reg)) {
		tab5KeyboardI2CStop();
		ESP_LOGW(TAG, "Tab5 Keyboard read reg 0x%02x: register NACK", reg);
		return ESP_ERR_NOT_FOUND;
	}
	tab5KeyboardI2CStart();
	if (!tab5KeyboardI2CWriteByte((tab5_kb_addr << 1) | 1)) {
		tab5KeyboardI2CStop();
		ESP_LOGW(TAG, "Tab5 Keyboard read reg 0x%02x: read address NACK", reg);
		return ESP_ERR_NOT_FOUND;
	}
	for (size_t i = 0; i < len; i++) {
		data[i] = tab5KeyboardI2CReadByte(i + 1 < len);
	}
	tab5KeyboardI2CStop();
	return ESP_OK;
}

static esp_err_t tab5KeyboardWriteReg(uint8_t reg, uint8_t value)
{
	tab5KeyboardI2CStart();
	if (!tab5KeyboardI2CWriteByte((tab5_kb_addr << 1) | 0)) {
		tab5KeyboardI2CStop();
		ESP_LOGW(TAG, "Tab5 Keyboard write reg 0x%02x: write address NACK", reg);
		return ESP_ERR_NOT_FOUND;
	}
	if (!tab5KeyboardI2CWriteByte(reg)) {
		tab5KeyboardI2CStop();
		ESP_LOGW(TAG, "Tab5 Keyboard write reg 0x%02x: register NACK", reg);
		return ESP_ERR_NOT_FOUND;
	}
	if (!tab5KeyboardI2CWriteByte(value)) {
		tab5KeyboardI2CStop();
		ESP_LOGW(TAG, "Tab5 Keyboard write reg 0x%02x: value NACK", reg);
		return ESP_ERR_NOT_FOUND;
	}
	tab5KeyboardI2CStop();
	return ESP_OK;
}

static int tab5KeyboardHidKeyToMask(uint8_t keycode, uint8_t modifier)
{
	return tab5KeyboardHidToMask(modifier, keycode);
}

static int tab5KeyboardMatrixToMask(uint8_t row, uint8_t col)
{
	static const uint8_t matrix_hid[5][14] = {
		{0x29, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2d, 0x2e, 0x4c},
		{0x35, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2f, 0x30, 0x31},
		{0x2b, 0x14, 0x1a, 0x08, 0x15, 0x17, 0x1c, 0x18, 0x0c, 0x12, 0x13, 0x33, 0x34, 0x2a},
		{0x00, 0x00, 0x04, 0x16, 0x07, 0x09, 0x0a, 0x0b, 0x0d, 0x0e, 0x0f, 0x52, 0x2d, 0x28},
		{0x00, 0x00, 0x1d, 0x1b, 0x06, 0x19, 0x05, 0x11, 0x10, 0x37, 0x50, 0x51, 0x4f, 0x2c},
	};

	if (row >= 5 || col >= 14) {
		return 0;
	}
	if (row == 4 && col == 0) {
		return TAB5_BTN_FIRE;   /* Ctrl */
	}
	if (row == 4 && col == 1) {
		return TAB5_BTN_STRAFE; /* Alt */
	}
	return tab5KeyboardHidKeyToMask(matrix_hid[row][col], 0);
}

static void tab5KeyboardLogErrorLimited(const char *message)
{
	TickType_t now = xTaskGetTickCount();
	if (now >= tab5_kb_next_error_log_tick) {
		tab5_kb_next_error_log_tick = now + pdMS_TO_TICKS(1000);
		ESP_LOGW(TAG, "%s", message);
	}
}

static bool tab5KeyboardScanAddress(void)
{
	bool found_keyboard = false;

	ESP_LOGI(TAG, "Scanning Tab5 Keyboard I2C bus on SDA=%d SCL=%d", TAB5_KEYBOARD_SDA, TAB5_KEYBOARD_SCL);
	for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
		if (tab5KeyboardI2CProbe(addr)) {
			ESP_LOGI(TAG, "I2C device found at 0x%02x", addr);
			if (addr == TAB5_KEYBOARD_ADDR) {
				found_keyboard = true;
			}
		}
	}
	tab5_kb_addr = TAB5_KEYBOARD_ADDR;
	if (!found_keyboard) {
		ESP_LOGW(TAG, "Tab5 Keyboard address 0x%02x not found", TAB5_KEYBOARD_ADDR);
	}
	return found_keyboard;
}

static int tab5KeyboardHidToMask(uint8_t modifier, uint8_t keycode)
{
	int mask = 0;

	if (modifier & 0x11) {
		mask |= TAB5_BTN_FIRE;   /* Ctrl */
	}
	if (modifier & 0x22) {
		mask |= TAB5_BTN_SPEED;  /* Shift */
	}
	if (modifier & 0x44) {
		mask |= TAB5_BTN_STRAFE; /* Alt */
	}

	switch (keycode) {
	case 0x04: /* A */
	case 0x50: /* Left */
		mask |= TAB5_BTN_LEFT;
		break;
	case 0x07: /* D */
	case 0x4f: /* Right */
		mask |= TAB5_BTN_RIGHT;
		break;
	case 0x16: /* S */
	case 0x51: /* Down */
		mask |= TAB5_BTN_DOWN;
		break;
	case 0x1a: /* W */
	case 0x52: /* Up */
		mask |= TAB5_BTN_UP;
		break;
	case 0x08: /* E */
	case 0x2c: /* Space */
		mask |= TAB5_BTN_USE;
		break;
	case 0x09: /* F */
		mask |= TAB5_BTN_FIRE;
		break;
	case 0x13: /* P */
		mask |= TAB5_BTN_PAUSE;
		break;
	case 0x27: /* 0 */
		mask |= TAB5_BTN_WEAPON;
		break;
	case 0x28: /* Enter */
		mask |= TAB5_BTN_FIRE;
		break;
	case 0x29: /* Escape */
		mask |= TAB5_BTN_ESCAPE;
		break;
	case 0x2b: /* Tab */
		mask |= TAB5_BTN_MAP;
		break;
	default:
		if (keycode >= 0x1e && keycode <= 0x26) {
			mask |= TAB5_BTN_WEAPON; /* 1..9 */
		}
		break;
	}

	return mask;
}

static void tab5KeyboardInit(void)
{
	gpio_config_t int_cfg = {
		.pin_bit_mask = 1ULL << TAB5_KEYBOARD_INT,
		.mode = GPIO_MODE_INPUT,
		.pull_up_en = GPIO_PULLUP_ENABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};
	gpio_config_t i2c_cfg = {
		.pin_bit_mask = (1ULL << TAB5_KEYBOARD_SDA) | (1ULL << TAB5_KEYBOARD_SCL),
		.mode = GPIO_MODE_INPUT_OUTPUT_OD,
		.pull_up_en = GPIO_PULLUP_ENABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};
	esp_err_t err;

	(void)gpio_config(&int_cfg);
	bsp_set_ext_5v_en(true);
	vTaskDelay(pdMS_TO_TICKS(20));

	if (!tab5_kb_bus_ready) {
		err = gpio_config(&i2c_cfg);
		if (err != ESP_OK) {
			ESP_LOGW(TAG, "Tab5 Keyboard GPIO I2C init failed: %s", esp_err_to_name(err));
			return;
		}
		tab5KeyboardSda(true);
		tab5KeyboardScl(true);
		tab5_kb_bus_ready = true;
		ESP_LOGI(TAG, "Tab5 Keyboard software I2C ready: SDA=%d SCL=%d INT=%d INT_level=%d addr=0x%02x",
		         TAB5_KEYBOARD_SDA, TAB5_KEYBOARD_SCL, TAB5_KEYBOARD_INT,
		         gpio_get_level(TAB5_KEYBOARD_INT), TAB5_KEYBOARD_ADDR);
		if (!tab5KeyboardScanAddress()) {
			tab5KeyboardDeinit();
			return;
		}
	}

	if (!tab5KeyboardI2CProbe(tab5_kb_addr)) {
		ESP_LOGW(TAG, "Tab5 Keyboard not detected at 0x%02x", tab5_kb_addr);
		tab5KeyboardDeinit();
		return;
	}

	uint8_t version = 0;
	err = tab5KeyboardReadReg(TAB5_KEYBOARD_REG_VERSION, &version, 1);
	if (err == ESP_OK) {
		ESP_LOGI(TAG, "Tab5 Keyboard firmware version: 0x%02x", version);
	} else {
		ESP_LOGW(TAG, "Tab5 Keyboard version read failed: %s", esp_err_to_name(err));
		tab5KeyboardDeinit();
		return;
	}

	/* Follow the Arduino UnitTab5Keyboard default: Normal mode, clear INT/queue, enable Normal INT. */
	err = tab5KeyboardWriteReg(TAB5_KEYBOARD_REG_MODE, 0);
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "Tab5 Keyboard failed to switch Normal mode: %s", esp_err_to_name(err));
		tab5KeyboardDeinit();
		return;
	}
	(void)tab5KeyboardWriteReg(TAB5_KEYBOARD_REG_INT_STAT, 0);
	(void)tab5KeyboardWriteReg(TAB5_KEYBOARD_REG_EVENT_NUM, 0);
	(void)tab5KeyboardWriteReg(TAB5_KEYBOARD_REG_INT_CFG, 0x01);
	tab5_kb_hid_mode = false;
	tab5_kb_ready = true;
	ESP_LOGI(TAG, "Tab5 Keyboard ready in Normal mode");
}

static int tab5ReadKeyboardMask(void)
{
	static int keyboard_mask;
	uint8_t event_num = 0;
	uint8_t event[2] = {0xff, 0xff};
	uint8_t raw_event = 0xff;

	if (!tab5_kb_ready) {
		TickType_t now = xTaskGetTickCount();
		if (now >= tab5_kb_next_probe_tick) {
			tab5_kb_next_probe_tick = now + pdMS_TO_TICKS(3000);
			tab5KeyboardInit();
		}
		return 0;
	}

	TickType_t now = xTaskGetTickCount();
	if (now < tab5_kb_next_poll_tick) {
		return keyboard_mask;
	}
	tab5_kb_next_poll_tick = now + pdMS_TO_TICKS(20);

	if (tab5KeyboardReadReg(TAB5_KEYBOARD_REG_EVENT_NUM, &event_num, 1) != ESP_OK) {
		tab5KeyboardLogErrorLimited("Tab5 Keyboard EVENT_NUM read failed");
		return keyboard_mask;
	}
	if (event_num) {
		ESP_LOGI(TAG, "Tab5 Keyboard event_num=%u", event_num);
	}
	while (event_num--) {
		if (!tab5_kb_hid_mode) {
			if (tab5KeyboardReadReg(TAB5_KEYBOARD_REG_KEY_EVENT, &raw_event, 1) != ESP_OK) {
				tab5KeyboardLogErrorLimited("Tab5 Keyboard normal event read failed");
				break;
			}
			if (raw_event == 0xff) {
				ESP_LOGI(TAG, "Tab5 Keyboard normal queue empty marker");
				break;
			}
			bool pressed = (raw_event & 0x80) != 0;
			uint8_t row = (raw_event >> 4) & 0x07;
			uint8_t col = raw_event & 0x0f;
			int key_mask = tab5KeyboardMatrixToMask(row, col);
			if (pressed) {
				keyboard_mask |= key_mask;
			} else {
				keyboard_mask &= ~key_mask;
			}
			ESP_LOGI(TAG, "Tab5 Keyboard NORMAL raw=0x%02x pressed=%u row=%u col=%u key_mask=0x%03x mask=0x%03x",
			         raw_event, pressed, row, col, key_mask, keyboard_mask);
			continue;
		}
		if (tab5KeyboardReadReg(TAB5_KEYBOARD_REG_HID_EVENT, event, sizeof(event)) != ESP_OK) {
			tab5KeyboardLogErrorLimited("Tab5 Keyboard HID event read failed");
			break;
		}
		if (event[0] == 0xff && event[1] == 0xff) {
			ESP_LOGI(TAG, "Tab5 Keyboard HID queue empty marker");
			break;
		}
		keyboard_mask = tab5KeyboardHidToMask(event[0], event[1]);
		ESP_LOGI(TAG, "Tab5 Keyboard HID mod=0x%02x key=0x%02x mask=0x%03x",
		         event[0], event[1], keyboard_mask);
	}

	return keyboard_mask;
}

static int tab5ReadTouchMask(void)
{
	lv_indev_t *indev = bsp_display_get_input_dev();
	lv_point_t point;
	int w;
	int h;
	int mask = 0;

	if (!indev) {
		return 0;
	}

#if LVGL_VERSION_MAJOR >= 9
	if (lv_indev_get_state(indev) != LV_INDEV_STATE_PRESSED) {
		return 0;
	}
	w = lv_display_get_horizontal_resolution(lv_display_get_default());
	h = lv_display_get_vertical_resolution(lv_display_get_default());
#else
	if (lv_indev_get_state(indev) != LV_INDEV_STATE_PR) {
		return 0;
	}
	w = lv_disp_get_hor_res(NULL);
	h = lv_disp_get_ver_res(NULL);
#endif
	lv_indev_get_point(indev, &point);

	if (point.y < h / 5) {
		if (point.x < w / 4) {
			return TAB5_BTN_ESCAPE;
		}
		if (point.x < w / 2) {
			return TAB5_BTN_MAP;
		}
		if (point.x < (w * 3) / 4) {
			return TAB5_BTN_PAUSE;
		}
		return TAB5_BTN_WEAPON;
	}

	if (point.x < w / 2) {
		int cx = w / 4;
		int cy = (h * 3) / 4;
		int dx = point.x - cx;
		int dy = point.y - cy;

		if (dy < -h / 10) mask |= TAB5_BTN_UP;
		if (dy > h / 10) mask |= TAB5_BTN_DOWN;
		if (dx < -w / 12) mask |= TAB5_BTN_LEFT;
		if (dx > w / 12) mask |= TAB5_BTN_RIGHT;
		return mask;
	}

	if (point.y > (h * 2) / 3) {
		mask |= (point.x > (w * 3) / 4) ? TAB5_BTN_FIRE : TAB5_BTN_USE;
	} else if (point.x > (w * 3) / 4) {
		mask |= TAB5_BTN_SPEED;
	} else {
		mask |= TAB5_BTN_STRAFE;
	}

	return mask;
}

static const JsKeyMap keymap[]={
	{TAB5_BTN_UP, &key_up},
	{TAB5_BTN_DOWN, &key_down},
	{TAB5_BTN_LEFT, &key_left},
	{TAB5_BTN_RIGHT, &key_right},
	{TAB5_BTN_USE, &key_use},
	{TAB5_BTN_FIRE, &key_fire},
	{TAB5_BTN_FIRE, &key_menu_enter},
	{TAB5_BTN_PAUSE, &key_pause},
	{TAB5_BTN_WEAPON, &key_weapontoggle},
	{TAB5_BTN_ESCAPE, &key_escape},
	{TAB5_BTN_MAP, &key_map},
	{TAB5_BTN_STRAFE, &key_strafe},
	{TAB5_BTN_SPEED, &key_speed},
	{0, NULL},
};

#else

//Mappings from PS2 buttons to keys
static const JsKeyMap keymap[]={
	{0x10, &key_up},
	{0x40, &key_down},
	{0x80, &key_left},
	{0x20, &key_right},
	
	{0x4000, &key_use},				//cross
	{0x2000, &key_fire},			//circle
	{0x2000, &key_menu_enter},		//circle
	{0x8000, &key_pause},			//square
	{0x1000, &key_weapontoggle},	//triangle

	{0x8, &key_escape},				//start
	{0x1, &key_map},				//select
	
	{0x400, &key_strafeleft},		//L1
	{0x100, &key_speed},			//L2
	{0x800, &key_straferight},		//R1
	{0x200, &key_strafe},			//R2

	{0, NULL},
};

#endif

void gamepadPoll(void)
{
	static int oldPollJsVal=
#if CONFIG_HW_M5STACK_TAB5
		0;
	int newJoyVal=tab5ReadTouchMask() | tab5ReadKeyboardMask();
#else
		0xffff;
	int newJoyVal=joyVal;
#endif
	event_t ev;

	for (int i=0; keymap[i].key!=NULL; i++) {
		if ((oldPollJsVal^newJoyVal)&keymap[i].mask) {
#if CONFIG_HW_M5STACK_TAB5
			ev.type=(newJoyVal&keymap[i].mask)?ev_keydown:ev_keyup;
#else
			ev.type=(newJoyVal&keymap[i].mask)?ev_keyup:ev_keydown;
#endif
			ev.data1=*keymap[i].key;
			D_PostEvent(&ev);
		}
	}

	oldPollJsVal=newJoyVal;
}


#if !CONFIG_HW_M5STACK_TAB5
void jsTask(void *arg) {
	int oldJoyVal=0xFFFF;
	printf("Joystick task starting.\n");
	while(1) {
		vTaskDelay(20/portTICK_PERIOD_MS);
		joyVal=psxReadInput();
//		if (joyVal!=oldJoyVal) printf("Joy: %x\n", joyVal^0xffff);
		oldJoyVal=joyVal;
	}
}
#endif

void gamepadInit(void)
{
	lprintf(LO_INFO, "gamepadInit: Initializing game pad.\n");
}

void jsInit() {
#if CONFIG_HW_M5STACK_TAB5
	tab5KeyboardInit();
	lprintf(LO_INFO, "jsInit: using M5Stack Tab5 touch and keyboard controls.\n");
#else
	//Starts the js task
	psxcontrollerInit();
	xTaskCreatePinnedToCore(&jsTask, "js", 5000, NULL, 7, NULL, 0);
#endif
}
