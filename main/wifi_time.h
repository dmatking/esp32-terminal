#pragma once

// Connect to WiFi, sync time via NTP, then disconnect.
// Call once at boot. Time persists in the system clock.
void wifi_time_sync(void);
