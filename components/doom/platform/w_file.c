//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//	WAD I/O backed by a flash partition.
//
// ---------------------------------------------------------------------------
// Why this file is short, and why W_CacheLumpNum is not patched
// ---------------------------------------------------------------------------
//
// Doom's WAD layer already has a zero-copy path built in, inherited from
// Chocolate Doom. In w_wad.c:
//
//     if (lump->wad_file->mapped != NULL)
//         result = lump->wad_file->mapped + lump->position;
//
// So if we hand the WAD layer a `mapped` pointer, every W_CacheLumpNum call
// returns a pointer straight into memory-mapped flash and no lump is ever
// copied into zone memory. Nothing in w_wad.c needs changing -- the work is
// entirely here, in producing that pointer.
//
// ---------------------------------------------------------------------------
// Why there is no sliding window
// ---------------------------------------------------------------------------
//
// The obvious fallback for "4MB will not map" is a sliding mmap window. It does
// not work here, and the reason is worth writing down.
//
// W_CacheLumpNum hands out raw pointers that callers hold across frames --
// texture composites, sprite patches and the status bar all keep them. Sliding
// the window invalidates pointers that are still live, with no way to find or
// fix them up. The failure would not be a clean error; it would be sporadic
// graphical corruption that migrates as the window slides.
//
// So the fallback is not a smaller window, it is a different strategy: leave
// `mapped` NULL and let w_wad.c take its ordinary path, reading lumps through
// esp_partition_read into zone memory where they stay put until Z_Free.
// Slower, and it costs zone memory, but correct.
//

#include <stdio.h>
#include <string.h>

#include "config.h"

#include "doomtype.h"
#include "i_system.h"
#include "m_argv.h"

#include "w_file.h"

#include "esp_log.h"
#include "esp_partition.h"

static const char *TAG = "doom.wad";

// Partition type/subtype for the WAD payload; these match partitions.csv.
// ESP-IDF reserves 0x00-0x3F for its own partition types and leaves 0x40-0xFE
// to the application, so 0x42 is ours to define.
#define WAD_PARTITION_TYPE     0x42
#define WAD_PARTITION_SUBTYPE  0x06

static const esp_partition_t *wad_partition;
static esp_partition_mmap_handle_t wad_map_handle;
static const void *wad_mapped_ptr;   // NULL => mmap unavailable, use read path

static wad_file_t wad_file;

// Map the WAD partition, falling back to the read path if the MMU cannot give
// us a window that large. Safe to call more than once.
static void W_InitFlashWad(void)
{
    if (wad_partition != NULL) {
        return;
    }

    wad_partition = esp_partition_find_first(WAD_PARTITION_TYPE, WAD_PARTITION_SUBTYPE, NULL);
    if (wad_partition == NULL) {
        I_Error("W_InitFlashWad: no WAD partition (type 0x%02x subtype 0x%02x) "
                "found in the partition table",
                WAD_PARTITION_TYPE, WAD_PARTITION_SUBTYPE);
    }

    // ESP_PARTITION_MMAP_DATA maps into the data address space, which permits
    // byte-aligned reads. The instruction-space variant (MMAP_INST) allows only
    // 4-byte-aligned access and would fault on Doom's unaligned lump reads.
    esp_err_t err = esp_partition_mmap(wad_partition, 0, wad_partition->size,
                                       ESP_PARTITION_MMAP_DATA,
                                       &wad_mapped_ptr, &wad_map_handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "WAD partition mapped: %lu bytes at %p (zero-copy lumps)",
                 (unsigned long)wad_partition->size, wad_mapped_ptr);
        return;
    }

    // ESP_ERR_NO_MEM here means MMU virtual address space, not heap. On the S3
    // the external-memory address range is shared between mapped flash and
    // PSRAM, so a large PSRAM configuration can crowd out a 4MB flash window.
    wad_mapped_ptr = NULL;
    ESP_LOGE(TAG, "esp_partition_mmap(%lu bytes) failed: %s",
             (unsigned long)wad_partition->size, esp_err_to_name(err));
    ESP_LOGW(TAG, "falling back to read path: lumps will be copied into zone "
                  "memory. Expect higher zone pressure and slower level loads.");
}

wad_file_t *W_OpenFile(char *path)
{
    (void)path;  // There is one WAD and it is the partition.

    W_InitFlashWad();

    wad_file.file_class = NULL;
    wad_file.mapped = (byte *)wad_mapped_ptr;   // NULL selects the copy path
    wad_file.length = wad_partition->size;

    return &wad_file;
}

void W_CloseFile(wad_file_t *wad)
{
    (void)wad;
    // The mapping lives as long as the game does. Unmapping it would invalidate
    // every lump pointer still held by the renderer.
}

size_t W_Read(wad_file_t *wad, unsigned int offset,
              void *buffer, size_t buffer_len)
{
    (void)wad;

    // Written to avoid overflow rather than as `offset + buffer_len > size`.
    // The WAD header is read straight off flash (w_wad.c), so a corrupt or
    // partially-flashed partition can yield a wild infotableofs; with the naive
    // form, offset near UINT_MAX wraps the sum to something small, the check
    // passes, and the memcpy below reads out of bounds and panics instead of
    // producing the clean I_Error this is here for.
    if (offset > wad_partition->size || buffer_len > wad_partition->size - offset) {
        I_Error("W_Read: read past end of WAD partition "
                "(offset %u, len %u, partition %lu)",
                offset, (unsigned)buffer_len, (unsigned long)wad_partition->size);
    }

    if (wad_mapped_ptr != NULL) {
        // Mapped: this is a memcpy out of the cache-backed flash window.
        memcpy(buffer, (const uint8_t *)wad_mapped_ptr + offset, buffer_len);
        return buffer_len;
    }

    esp_err_t err = esp_partition_read(wad_partition, offset, buffer, buffer_len);
    if (err != ESP_OK) {
        I_Error("W_Read: esp_partition_read at offset %u failed: %s",
                offset, esp_err_to_name(err));
    }
    return buffer_len;
}
