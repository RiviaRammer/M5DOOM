#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_spiffs.h"
#include "tab5_storage.h"

void tab5_storage_init(void)
{
    const esp_vfs_spiffs_conf_t conf = {
        .base_path = "/save",
        .partition_label = "saves",
        .max_files = 4,
        /* Never silently erase existing saves after a mount error. Reflash
         * the empty saves image explicitly to recover a damaged filesystem. */
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE("doom_save", "Mount failed: %s; saves unavailable (no automatic format)",
                 esp_err_to_name(err));
        return;
    }
    /* SPIFFS is flat and stat("/save") does not report a real directory.
     * Use PrBoom's existing environment override instead of its -save option,
     * whose desktop-only directory check would reject a valid VFS mount. */
    if (setenv("DOOMSAVEDIR", "/save", 1) != 0) {
        ESP_LOGE("doom_save", "Cannot set save directory");
        return;
    }

    /* Recover an overwrite interrupted after the old slot became a backup.
     * Completed writes are closed before promotion; do not promote .tmp. */
    for (int slot = 0; slot < 8; ++slot) {
        char path[40], backup[44], temporary[44];
        struct stat st;
        snprintf(path, sizeof(path), "/save/prbmsav%d.dsg", slot);
        snprintf(backup, sizeof(backup), "%s.bak", path);
        snprintf(temporary, sizeof(temporary), "%s.tmp", path);
        if (stat(path, &st) != 0 && errno == ENOENT) {
            if (rename(backup, path) == 0) {
                ESP_LOGW("doom_save", "Recovered slot %d from backup", slot + 1);
            }
        }
        if (stat(path, &st) == 0) remove(backup);
        remove(temporary);
    }
    size_t total = 0, used = 0;
    if (esp_spiffs_info("saves", &total, &used) == ESP_OK) {
        ESP_LOGI("doom_save", "Flash saves mounted at /save: used=%u total=%u; full flash resets saves",
                 (unsigned)used, (unsigned)total);
    }
}
