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

// microSD card on the display's SPI bus (Waveshare ESP32-C6-LCD-1.47).
// Mounted as FAT at SD_CARD_MOUNT; FAT32/FAT16 only (exFAT is off).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SD_CARD_MOUNT "/sd"

// Register the card on an SPI bus that is already initialised (with MISO) and
// try to mount it. A missing card is not an error: later calls retry.
void sd_card_init(int spi_host, int cs_gpio);

// Mount the card if it is not mounted. Blocks for up to about a second when
// no card is present, so call it from a worker task, never the Noise task.
bool sd_card_ensure_mounted(void);

// Forget the mount after an I/O error (card pulled), so the next call remounts.
void sd_card_unmount(void);

// Check a card-relative path ("photos/cat.jpg", "/a.jpg" or "" for the root)
// and build its absolute VFS path into `out`. Rejects "..", backslashes and
// control characters. Returns false with a reason in *why.
bool sd_card_resolve(const char *relative, char *out, size_t out_len, const char **why);

// Start an async listing of a card directory. Calls noise_ctrl_send_command_result
// itself. Returns false with code/message when the task can't start.
bool sd_card_list_start(const char *relative, uint64_t session_generation,
                        const char *request_id, const char **code, const char **message);

#ifdef __cplusplus
}
#endif
