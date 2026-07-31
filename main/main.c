//
// Entry point for DOOM on the Waveshare ESP32-S3-RLCD-4.2.
//
// This is deliberately thin. ESP_DOOM did the WAD partition mmap here, with a
// comment explaining that esp_partition could not be included from a component
// -- that was a missing REQUIRES in the component's CMakeLists, not a real
// restriction. The mapping now lives in the doom component's w_file.c, next to
// the code that consumes it.
//

#include <stdio.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "doom.main";

extern int doom_main(int argc, char **argv);

// Doom's renderer recurses through BSP traversal and keeps sizeable locals, so
// it needs far more stack than a default FreeRTOS task gets. ESP_DOOM used
// 22480 bytes on ESP32-classic; we start higher because we have the RAM and a
// stack overflow here shows up as an unhelpful corruption crash rather than a
// clean fault.
#define DOOM_TASK_STACK_BYTES  (32 * 1024)

static void run_doom(void *arg)
{
    (void)arg;

    // -mb sets the zone heap size in megabytes. Zone memory is Doom's own
    // allocator; everything the game allocates at runtime comes out of it.
    char *argv[] = { "doom", "-mb", "3", "-iwad", "doom1.wad", NULL };
    int argc = (int)(sizeof(argv) / sizeof(argv[0])) - 1;

    doom_main(argc, argv);

    // D_DoomMain is not supposed to return.
    ESP_LOGE(TAG, "doom_main returned unexpectedly");
    vTaskDelete(NULL);
}

void app_main(void)
{
    // NVS is not used by the game itself, but ESP-IDF subsystems expect it to be
    // initialised and it is cheap to do here.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_LOGI(TAG, "boot: internal heap %u free, PSRAM %u free",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // Pinned to core 0. The game loop is single-threaded and latency-sensitive;
    // leaving core 1 free keeps the (future) SPI panel flush and I2S audio feed
    // off the same core as the renderer.
    //
    // Priority 5, not configMAX_PRIORITIES-1. ESP_DOOM used the maximum, which
    // puts the game above esp_timer (22) and every IDF service task; a busy
    // frame then starves core-0 services, and with the idle-task watchdog
    // disabled on this core there is nothing left to complain about it.
    xTaskCreatePinnedToCore(run_doom, "doom", DOOM_TASK_STACK_BYTES, NULL,
                            5, NULL, 0);
}
