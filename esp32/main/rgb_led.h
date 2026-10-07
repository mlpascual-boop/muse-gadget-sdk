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

// Onboard WS2812 RGB LED (Waveshare ESP32-C6-LCD-1.47, GPIO8). Mirrors the
// status colours by default; Muse can take it over with led.set and hand it
// back with led.clear or when the override's time runs out.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RGB_LED_SOLID = 0,
    RGB_LED_BLINK,
    RGB_LED_BREATHE,
} rgb_led_effect_t;

bool rgb_led_init(int gpio);

// Status colour from the LED task, already including breathe/blink frames.
// Shown unless an override is active.
void rgb_led_status(uint8_t r, uint8_t g, uint8_t b);

// Override from Muse. brightness_pct 1-100; seconds 0 = until rgb_led_clear().
void rgb_led_override(uint8_t r, uint8_t g, uint8_t b, rgb_led_effect_t effect,
                      int brightness_pct, uint32_t seconds);
void rgb_led_clear(void);

// Parse "red", "#ff8800" or "ff8800". Returns false if not recognised.
bool rgb_led_parse_color(const char *s, uint8_t *r, uint8_t *g, uint8_t *b);

#ifdef __cplusplus
}
#endif
