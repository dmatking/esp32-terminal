#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "display.h"

// Initialise the BLE keyboard host and wire it to the display.
// Returns a queue from which ASCII bytes can be read.
// If force_repair is true, clear all BLE bonds and do a fresh scan.
QueueHandle_t bt_kbd_ui_init(display_t *display, bool force_repair);

// Block until the keyboard is connected.
void bt_kbd_ui_wait_connected(void);

// Suppress BLE status overlays while a terminal session is active.
// When true, keyboard disconnects are silent — the terminal stays on screen.
void bt_kbd_ui_set_terminal_active(bool active);
