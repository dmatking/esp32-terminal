#pragma once

// Name prefix to match during BLE scan (keyboard changes its random address
// between pairing sessions, so we match by name instead of MAC).
#define BT_KBD_NAME "MX MCHNCL"

// Set to 1 to scan and print all devices instead of connecting.
#define BT_KBD_SCAN_MODE 0
