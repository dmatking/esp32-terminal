#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "display.h"
#include "ssh_targets.h"

// Initialize wolfSSH (call once at startup).
void ssh_term_init(void);

// Connect to an SSH target and run interactive terminal on the display.
// keys: queue of char from bt_kbd_init().
// Returns when the connection closes or fails.
void ssh_term_connect(display_t *display, QueueHandle_t keys,
                      const ssh_target_t *target);
