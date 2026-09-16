#include <stdbool.h>
#include <stdint.h>

bool spi_lcd_frame_due(void);
void spi_lcd_wait_finish();
void spi_lcd_send(const uint8_t *scr);
void spi_lcd_init();
