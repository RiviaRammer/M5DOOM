#pragma once

/* SPIFFS is mounted separately from the memory-mapped WAD partition. */
void tab5_storage_init(void);

#ifdef TAB5_SAVE_SELFTEST
void tab5_save_selftest_poll(void);
#endif
