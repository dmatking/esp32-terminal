#pragma once

#include "display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// Run the SSH target menu. Shows list of saved targets, allows
// add/edit/delete. When user selects a target, connects via SSH.
// Returns to menu on disconnect. Never returns.
void menu_run(display_t *display, QueueHandle_t keys);
