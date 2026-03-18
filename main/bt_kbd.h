#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "display.h"

// Initialize BLE HID Host, start scanning/connecting to a keyboard.
// Passkey (if required for pairing) is shown on the display.
// Returns a queue from which ASCII bytes can be read.
// If force_repair is true, clear all BLE bonds and do a fresh pairing scan.
QueueHandle_t bt_kbd_init(display_t *display, bool force_repair);

// Block until the keyboard is connected (or forever if it never connects).
void bt_kbd_wait_connected(void);
