// Copyright 2025 David M. King
// SPDX-License-Identifier: Apache-2.0
//
// GT911 capacitive touch controller — I2C on GPIO 7/8

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// Initialize GT911 on I2C bus (initializes I2C_NUM_0 internally).
esp_err_t touch_init(void);

// Poll for a completed tap. Returns true once per tap, sets *x/*y to
// the pixel coordinates of the tap start position. Call at ~10-30 Hz.
bool touch_poll_tap(uint16_t *x, uint16_t *y);
