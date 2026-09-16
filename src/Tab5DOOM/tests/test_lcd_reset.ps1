param(
    [string]$Compiler = 'gcc',
    [string]$Output = "$env:TEMP/tab5_lcd_reset_test.exe"
)
$ErrorActionPreference = 'Stop'
$bsp = Get-Content -Raw -LiteralPath "$PSScriptRoot/../components/m5stack_tab5/m5stack_tab5.c"
$init = [regex]::Match($bsp, '(?s)void bsp_io_expander_pi4ioe_init\([^\n]*\)\s*\{.*?(?=\r?\nvoid bsp_set_charge_qc_en)').Value
$reset = [regex]::Match($bsp, '(?s)void bsp_reset_tp\(\)\s*\{.*?(?=\r?\n//={10})').Value
if (!$init -or !$reset) { throw 'Cannot locate BSP reset functions' }
$prefix = @'
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int i2c_master_bus_handle_t;
typedef int i2c_master_dev_handle_t;
typedef struct { int dev_addr_length, device_address, scl_speed_hz; } i2c_device_config_t;
static int i2c_dev_handle_pi4ioe1, i2c_dev_handle_pi4ioe2;
static uint8_t registers[2][256];
static unsigned asserted_delays, released_delays;
#define I2C_ADDR_BIT_LEN_7 7
#define I2C_DEV_ADDR_PI4IOE1 0x43
#define I2C_DEV_ADDR_PI4IOE2 0x44
#define I2C_MASTER_TIMEOUT_MS 50
#define PI4IO_REG_CHIP_RESET 0x01
#define PI4IO_REG_IO_DIR 0x03
#define PI4IO_REG_OUT_SET 0x05
#define PI4IO_REG_OUT_H_IM 0x07
#define PI4IO_REG_IN_DEF_STA 0x09
#define PI4IO_REG_PULL_EN 0x0b
#define PI4IO_REG_PULL_SEL 0x0d
#define PI4IO_REG_INT_MASK 0x11
#define GPIO_NUM_23 23
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERROR_CHECK(expr) assert((expr) == ESP_OK)
#define ESP_LOGI(...) ((void)0)
#define pdMS_TO_TICKS(ms) (ms)
#define setbit(x,y) ((x) |= (1u << (y)))
#define clrbit(x,y) ((x) &= ~(1u << (y)))
static void gpio_reset_pin(int pin) { (void)pin; }
static int i2c_master_bus_add_device(int bus, const i2c_device_config_t *cfg, int *dev) {
    (void)bus; *dev = cfg->device_address - 0x43; return 0;
}
static int i2c_master_transmit(int dev, const uint8_t *data, unsigned len, int timeout) {
    (void)timeout; assert(len == 2);
    if (data[0] == PI4IO_REG_CHIP_RESET) memset(registers[dev], 0, 256);
    else registers[dev][data[0]] = data[1];
    /* Check every intermediate write, not just the final configuration. */
    assert(!(registers[0][PI4IO_REG_IO_DIR] & registers[0][PI4IO_REG_OUT_SET] & 0x10));
    return 0;
}
static int i2c_master_transmit_receive(int dev, const uint8_t *reg, unsigned txlen,
                                     uint8_t *value, unsigned rxlen, int timeout) {
    (void)timeout; assert(txlen == 1 && rxlen == 1); *value = registers[dev][*reg]; return 0;
}
static void vTaskDelay(unsigned ticks) {
    assert(ticks >= 10);
    if (registers[0][PI4IO_REG_IO_DIR] & 0x10) asserted_delays++;
    else released_delays++;
}
'@
$suffix = @'
int main(void) {
    bsp_io_expander_pi4ioe_init(0);
    assert(registers[0][PI4IO_REG_IO_DIR] == 0x6f);
    assert(registers[0][PI4IO_REG_OUT_SET] == 0x66);
    assert(registers[0][PI4IO_REG_PULL_EN] == 0x7f);
    assert(registers[0][PI4IO_REG_PULL_SEL] == 0x7f);
    assert(asserted_delays == 1 && released_delays == 1);
    for (unsigned state = 0; state < 256; state++) {
        uint8_t initial = state & ~0x10;
        registers[0][PI4IO_REG_IO_DIR] = initial;
        registers[0][PI4IO_REG_OUT_SET] = initial;
        bsp_reset_tp();
        assert(registers[0][PI4IO_REG_IO_DIR] == initial);
        assert(registers[0][PI4IO_REG_OUT_SET] == (initial | 0x20));
    }
    assert(asserted_delays == 257 && released_delays == 257);
    puts("PASS: actual BSP reset sequences never drive LCD_RST high; other pins preserved");
}
'@
($prefix + "`n" + $init + "`n" + $reset + "`n" + $suffix) |
    & $Compiler -std=c11 -Wall -Wextra -Werror -x c - -o $Output
if ($LASTEXITCODE -ne 0) { throw 'LCD reset test compilation failed' }
& $Output
if ($LASTEXITCODE -ne 0) { throw 'LCD reset sequence test failed' }
