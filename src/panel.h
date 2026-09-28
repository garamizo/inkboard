#pragma once
#include <stdint.h>

// Each call powers the HAT, draws with one full refresh, hibernates, and powers it off.
void panel_show(const uint8_t* frame);                          // 48,000-byte server frame
void panel_show_error(const char* message, const char* query);  // config error screen
