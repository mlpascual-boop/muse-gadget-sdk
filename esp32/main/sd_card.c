/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "sd_card.h"

#if CONFIG_HOMEHUB_SD_CARD

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "driver/sdspi_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "noise_control.h"
#include "sdmmc_cmd.h"

static const char *TAG = "link.sd";

// Shared with the 40 MHz LCD; SD cards in SPI mode are specified to 20 MHz.
#define SD_FREQ_KHZ       SDMMC_FREQ_DEFAULT
#define SD_LIST_MAX       60
#define SD_LIST_STACK     4096
#define SD_PATH_MAX       160

static SemaphoreHandle_t s_lock;
static int s_host = -1;
static int s_cs = -1;
static sdmmc_card_t *s_card;

static bool mount_locked(void) {
    if (s_card) return true;
    if (s_host < 0) return false;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = s_host;
    host.max_freq_khz = SD_FREQ_KHZ;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = s_host;
    slot.gpio_cs = s_cs;
    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 2,
        .allocation_unit_size = 16 * 1024,
    };
    esp_err_t err = esp_vfs_fat_sdspi_mount(SD_CARD_MOUNT, &host, &slot, &mount, &s_card);
    if (err != ESP_OK) {
        s_card = NULL;
        ESP_LOGW(TAG, "no card mounted: %s%s", esp_err_to_name(err),
                 err == ESP_FAIL ? " (not FAT32/FAT16?)" : "");
        return false;
    }
    ESP_LOGI(TAG, "card mounted at " SD_CARD_MOUNT ": %s, %llu MB", s_card->cid.name,
             ((unsigned long long)s_card->csd.capacity) * s_card->csd.sector_size / (1024 * 1024));
    return true;
}

void sd_card_init(int spi_host, int cs_gpio) {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return;
    s_host = spi_host;
    s_cs = cs_gpio;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    mount_locked();
    xSemaphoreGive(s_lock);
}

bool sd_card_ensure_mounted(void) {
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = mount_locked();
    xSemaphoreGive(s_lock);
    return ok;
}

void sd_card_unmount(void) {
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_card) {
        esp_vfs_fat_sdcard_unmount(SD_CARD_MOUNT, s_card);
        s_card = NULL;
        ESP_LOGW(TAG, "card unmounted after an error; will remount on next use");
    }
    xSemaphoreGive(s_lock);
}

bool sd_card_resolve(const char *relative, char *out, size_t out_len, const char **why) {
    if (!relative) relative = "";
    while (*relative == '/') relative++;
    for (const char *p = relative; *p; p++) {
        if ((unsigned char)*p < 0x20 || *p == '\\') {
            *why = "path has a backslash or control character";
            return false;
        }
    }
    // Reject any ".." segment.
    for (const char *p = relative; *p;) {
        const char *end = strchr(p, '/');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len == 2 && p[0] == '.' && p[1] == '.') {
            *why = "path may not contain ..";
            return false;
        }
        if (!end) break;
        p = end + 1;
    }
    int n = snprintf(out, out_len, SD_CARD_MOUNT "/%s", relative);
    if (n < 0 || (size_t)n >= out_len) {
        *why = "path is too long";
        return false;
    }
    // Drop a trailing slash, except on the mount root itself.
    size_t len = strlen(out);
    while (len > sizeof(SD_CARD_MOUNT) && out[len - 1] == '/') out[--len] = '\0';
    return true;
}

// ---- sd.list -----------------------------------------------------------------

typedef struct {
    noise_ctrl_session_generation_t session_generation;
    char request_id[64];
    char path[SD_PATH_MAX];
} list_args_t;

static cJSON *list_error(const char *code, const char *message) {
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", false);
    cJSON *error = cJSON_AddObjectToObject(result, "error");
    cJSON_AddStringToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    return result;
}

static cJSON *list_dir(const char *path) {
    if (!sd_card_ensure_mounted()) {
        return list_error("no_card", "no microSD card mounted; insert a FAT32 card");
    }
    DIR *dir = opendir(path);
    if (!dir) {
        struct stat st;
        if (stat(SD_CARD_MOUNT "/.", &st) != 0) sd_card_unmount();  // card pulled
        return list_error("not_found", "no such directory on the card");
    }
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON *payload = cJSON_AddObjectToObject(result, "payload");
    cJSON_AddStringToObject(payload, "path", path + strlen(SD_CARD_MOUNT));
    cJSON *entries = cJSON_AddArrayToObject(payload, "entries");
    int count = 0;
    bool truncated = false;
    char full[SD_PATH_MAX + 260];
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (count == SD_LIST_MAX) {
            truncated = true;
            break;
        }
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", de->d_name);
        bool is_dir = de->d_type == DT_DIR;
        cJSON_AddBoolToObject(e, "dir", is_dir);
        if (!is_dir) {
            struct stat st;
            snprintf(full, sizeof(full), "%s/%s", path, de->d_name);
            if (stat(full, &st) == 0) cJSON_AddNumberToObject(e, "bytes", (double)st.st_size);
        }
        cJSON_AddItemToArray(entries, e);
        count++;
    }
    closedir(dir);
    cJSON_AddBoolToObject(payload, "truncated", truncated);
    uint64_t total = 0, free_bytes = 0;
    if (esp_vfs_fat_info(SD_CARD_MOUNT, &total, &free_bytes) == ESP_OK) {
        cJSON_AddNumberToObject(payload, "card_mb", (double)(total / (1024 * 1024)));
        cJSON_AddNumberToObject(payload, "free_mb", (double)(free_bytes / (1024 * 1024)));
    }
    return result;
}

static void list_task(void *arg) {
    list_args_t *a = arg;
    cJSON *result = list_dir(a->path);
    noise_ctrl_send_command_result(a->session_generation, a->request_id, result);
    free(a);
    vTaskDelete(NULL);
}

bool sd_card_list_start(const char *relative, uint64_t session_generation,
                        const char *request_id, const char **code, const char **message) {
    list_args_t *a = calloc(1, sizeof(*a));
    if (!a) {
        *code = "out_of_memory";
        *message = "failed to allocate";
        return false;
    }
    if (!sd_card_resolve(relative, a->path, sizeof(a->path), message)) {
        free(a);
        *code = "invalid_params";
        return false;
    }
    a->session_generation = session_generation;
    strncpy(a->request_id, request_id, sizeof(a->request_id) - 1);
    if (xTaskCreate(list_task, "sd_list", SD_LIST_STACK, a, 4, NULL) != pdPASS) {
        free(a);
        *code = "out_of_memory";
        *message = "failed to start the listing task";
        return false;
    }
    return true;
}

#endif  // CONFIG_HOMEHUB_SD_CARD
