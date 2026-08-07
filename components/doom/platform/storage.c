//
// Persistent storage for savegames and the config file.
//
// Doom writes savegames and default.cfg into whatever directory
// M_SetConfigDir is given. Without a mounted filesystem those writes fail
// silently -- the boot log says "Using . for configuration and saves" and
// nothing is ever persisted.
//
// This mounts the `storage` partition already reserved in partitions.csv (7MB,
// FAT) at /fat. Internal flash rather than the SD card on purpose: the card's
// CS line runs through R7, which the schematic marks NC, so whether a card
// enumerates at all is an open hardware question. Savegames are a few hundred
// KB, which does not justify debugging an unpopulated resistor -- and the
// partition is already allocated and otherwise unused.
//
// Wear levelling is on because Doom rewrites default.cfg on every exit and
// savegames land in the same few sectors.
//

#include "storage.h"

#include <sys/stat.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"

static const char *TAG = "doom.fs";

static wl_handle_t s_wl = WL_INVALID_HANDLE;

const char *Storage_Mount(void)
{
    if (s_wl != WL_INVALID_HANDLE) {
        return STORAGE_MOUNT_POINT "/";
    }

    const esp_vfs_fat_mount_config_t cfg = {
        // Format on first boot: the partition ships blank, so without this the
        // very first mount fails and savegames never work on a fresh board.
        .format_if_mount_failed = true,
        .max_files = 4,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
    };

    esp_err_t e = esp_vfs_fat_spiflash_mount_rw_wl(STORAGE_MOUNT_POINT,
                                                   "storage", &cfg, &s_wl);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s -- savegames will not persist",
                 esp_err_to_name(e));
        s_wl = WL_INVALID_HANDLE;
        return NULL;
    }

    uint64_t total = 0, used = 0;
    esp_vfs_fat_info(STORAGE_MOUNT_POINT, &total, &used);
    ESP_LOGI(TAG, "mounted %s -- %llu KB total, %llu KB used",
             STORAGE_MOUNT_POINT, total / 1024, used / 1024);

    // Doom appends its own subdirectory to this path, but only creates it if
    // the parent exists.
    mkdir(STORAGE_MOUNT_POINT "/doom", 0777);

    // Trailing slash matters: M_GetSaveGameDir concatenates directly onto it.
    return STORAGE_MOUNT_POINT "/doom/";
}
