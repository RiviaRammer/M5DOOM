/* Developer-only hardware regression, compiled only with
 * -D TAB5_SAVE_SELFTEST=ON. Uses slot 8 and reboots the device once. */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#undef true
#undef false
#include "doomstat.h"
#include "g_game.h"
#include "tab5_storage.h"

extern void M_ReadSaveStrings(void);
extern char savegamestrings[10][24];

static const char *TAG = "save_selftest";
static const char *marker = "/save/selftest.state";
static const char *slot_path = "/save/prbmsav7.dsg";
static struct { int x, y, health, armor, ammo; } expected;

void tab5_save_selftest_poll(void)
{
    static int phase;
    static TickType_t deadline;
    player_t *p = &players[consoleplayer];
    if (phase < 0) return;
    if (gamestate != GS_LEVEL || !usergame || demoplayback || !p->mo) return;
    TickType_t now = xTaskGetTickCount();
    if (!phase) {
        FILE *f = fopen(marker, "rb");
        if (f) {
            int ok = fread(&expected, sizeof(expected), 1, f) == 1;
            fclose(f);
            if (!ok) { ESP_LOGE(TAG, "FAIL: marker read"); phase = -1; return; }
            ESP_LOGI(TAG, "Reboot persistence confirmed; requesting slot 8 load");
            G_LoadGame(7, 0);
            phase = 2;
        } else {
            p->cheats |= CF_GODMODE;
            p->health = p->mo->health = 73;
            p->armorpoints = 19;
            p->ammo[am_clip] = 37;
            expected.x = p->mo->x;
            expected.y = p->mo->y;
            expected.health = 73;
            expected.armor = 19;
            expected.ammo = 37;
            ESP_LOGI(TAG, "Requesting real engine save to slot 8");
            G_SaveGame(7, "FLASH SELFTEST");
            phase = 1;
        }
        deadline = now + pdMS_TO_TICKS(3000);
        return;
    }
    if ((int32_t)(now - deadline) < 0) return;
    if (phase == 1) {
        struct stat st;
        if (stat(slot_path, &st) != 0 || st.st_size < 128) {
            ESP_LOGE(TAG, "FAIL: save file absent/short"); phase = -1; return;
        }
        FILE *f = fopen(marker, "wb");
        int ok = f && fwrite(&expected, sizeof(expected), 1, f) == 1;
        if (f && fclose(f) != 0) ok = 0;
        if (!ok) { ESP_LOGE(TAG, "FAIL: marker write"); phase = -1; return; }
        ESP_LOGI(TAG, "Save complete (%ld bytes); rebooting to test persistence", (long)st.st_size);
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    } else if (phase == 2 || phase == 4) {
        int ok = p->health == expected.health && p->armorpoints == expected.armor &&
                 p->ammo[am_clip] == expected.ammo && p->mo->x == expected.x && p->mo->y == expected.y;
        M_ReadSaveStrings();
        ok = ok && strcmp(savegamestrings[7], "FLASH SELFTEST") == 0;
        ESP_LOGI(TAG, "%s: %s/load + menu health=%d armor=%d ammo=%d position=%ld,%ld",
                 ok ? "PASS" : "FAIL", phase == 2 ? "reboot" : "overwrite",
                 p->health, p->armorpoints, p->ammo[am_clip],
                 (long)p->mo->x, (long)p->mo->y);
        if (ok && phase == 2) {
            p->health = p->mo->health = expected.health = 51;
            p->armorpoints = expected.armor = 29;
            p->ammo[am_clip] = expected.ammo = 47;
            G_SaveGame(7, "FLASH SELFTEST");
            phase = 3;
            deadline = now + pdMS_TO_TICKS(3000);
            return;
        }
        if (ok) {
            FILE *f = fopen(marker, "wb");
            int written = f && fwrite(&expected, sizeof(expected), 1, f) == 1;
            if (f && fclose(f) != 0) written = 0;
            if (!written) ESP_LOGE(TAG, "FAIL: final marker write");
        }
        /* Keep slot 8/marker for subsequent app-flash persistence validation.
         * A final full production flash must wipe both. */
        phase = -1;
    } else if (phase == 3) {
        p->health = p->mo->health = 99;
        p->armorpoints = 0;
        p->ammo[am_clip] = 1;
        G_LoadGame(7, 0);
        phase = 4;
        deadline = now + pdMS_TO_TICKS(3000);
    }
}
