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

#include "rgb_led.h"

#include "sdkconfig.h"

#if CONFIG_HOMEHUB_RGB_LED

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led_strip.h"

static const char *TAG = "link.rgb";

#define RGB_TICK_MS        20
#define RGB_BLINK_MS       500
#define RGB_BREATHE_MS     2000
// Status colours are tuned to peak at 80; halve them for a calm indicator.
#define RGB_STATUS_PCT     50

static led_strip_handle_t s_strip;
static SemaphoreHandle_t s_lock;
static uint8_t s_status[3];
static bool s_override;
static uint8_t s_ov[3];
static rgb_led_effect_t s_effect;
static int64_t s_ov_start_us, s_ov_end_us;  // end 0 = no timeout
static TaskHandle_t s_task;

static void write_locked(uint8_t r, uint8_t g, uint8_t b) {
    if (!s_strip) return;
    if (led_strip_set_pixel(s_strip, 0, r, g, b) == ESP_OK) (void)led_strip_refresh(s_strip);
}

// Renders overrides; idle (blocked) when there's none.
static void rgb_task(void *arg) {
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool active = s_override;
        if (active && s_ov_end_us && esp_timer_get_time() >= s_ov_end_us) {
            s_override = active = false;
            write_locked(s_status[0], s_status[1], s_status[2]);
            ESP_LOGI(TAG, "override expired; back to status");
        }
        if (active) {
            float level = 1.0f;
            int64_t t_ms = (esp_timer_get_time() - s_ov_start_us) / 1000;
            if (s_effect == RGB_LED_BLINK) {
                level = (t_ms / RGB_BLINK_MS) % 2 ? 0.0f : 1.0f;
            } else if (s_effect == RGB_LED_BREATHE) {
                level = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * (float)(t_ms % RGB_BREATHE_MS)
                                           / RGB_BREATHE_MS);
            }
            write_locked((uint8_t)(s_ov[0] * level), (uint8_t)(s_ov[1] * level),
                         (uint8_t)(s_ov[2] * level));
        }
        xSemaphoreGive(s_lock);
        if (active) vTaskDelay(pdMS_TO_TICKS(RGB_TICK_MS));
        else ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}

bool rgb_led_init(int gpio) {
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return false;
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = gpio,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
#if CONFIG_HOMEHUB_RGB_LED_RED_FIRST
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB,
#else
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
#endif
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RGB LED init failed on GPIO%d: %s", gpio, esp_err_to_name(err));
        s_strip = NULL;
        return false;
    }
    write_locked(0, 0, 0);
    if (xTaskCreate(rgb_task, "rgb_led", 2560, NULL, 2, &s_task) != pdPASS) {
        ESP_LOGW(TAG, "RGB LED task failed to start");
        s_task = NULL;
    }
    ESP_LOGI(TAG, "RGB LED ready on GPIO%d", gpio);
    return true;
}

void rgb_led_status(uint8_t r, uint8_t g, uint8_t b) {
    if (!s_lock) return;
    r = (uint8_t)(r * RGB_STATUS_PCT / 100);
    g = (uint8_t)(g * RGB_STATUS_PCT / 100);
    b = (uint8_t)(b * RGB_STATUS_PCT / 100);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool changed = r != s_status[0] || g != s_status[1] || b != s_status[2];
    s_status[0] = r;
    s_status[1] = g;
    s_status[2] = b;
    if (changed && !s_override) write_locked(r, g, b);
    xSemaphoreGive(s_lock);
}

void rgb_led_override(uint8_t r, uint8_t g, uint8_t b, rgb_led_effect_t effect,
                      int brightness_pct, uint32_t seconds) {
    if (!s_lock) return;
    if (brightness_pct < 1) brightness_pct = 1;
    if (brightness_pct > 100) brightness_pct = 100;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ov[0] = (uint8_t)(r * brightness_pct / 100);
    s_ov[1] = (uint8_t)(g * brightness_pct / 100);
    s_ov[2] = (uint8_t)(b * brightness_pct / 100);
    s_effect = effect;
    s_ov_start_us = esp_timer_get_time();
    s_ov_end_us = seconds ? s_ov_start_us + (int64_t)seconds * 1000000 : 0;
    s_override = true;
    write_locked(s_ov[0], s_ov[1], s_ov[2]);
    xSemaphoreGive(s_lock);
    if (s_task) xTaskNotifyGive(s_task);
}

void rgb_led_clear(void) {
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_override = false;
    write_locked(s_status[0], s_status[1], s_status[2]);
    xSemaphoreGive(s_lock);
}

#endif  // CONFIG_HOMEHUB_RGB_LED

// ---- Colour parsing (host-testable, no ESP dependencies) -------------------

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

bool rgb_led_parse_color(const char *s, uint8_t *r, uint8_t *g, uint8_t *b) {
    static const struct { const char *name; uint8_t r, g, b; } names[] = {
        {"red", 255, 0, 0},       {"green", 0, 255, 0},     {"blue", 0, 0, 255},
        {"yellow", 255, 180, 0},  {"orange", 255, 80, 0},   {"purple", 160, 0, 255},
        {"pink", 255, 40, 120},   {"white", 255, 255, 255}, {"cyan", 0, 255, 255},
        {"magenta", 255, 0, 255}, {"off", 0, 0, 0},
    };
    if (!s) return false;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcasecmp(s, names[i].name) == 0) {
            *r = names[i].r;
            *g = names[i].g;
            *b = names[i].b;
            return true;
        }
    }
    if (*s == '#') s++;
    if (strlen(s) != 6) return false;
    for (int i = 0; i < 6; i++) {
        if (!isxdigit((unsigned char)s[i])) return false;
    }
    unsigned long v = strtoul(s, NULL, 16);
    *r = (uint8_t)(v >> 16);
    *g = (uint8_t)(v >> 8);
    *b = (uint8_t)v;
    return true;
}
