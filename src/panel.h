#pragma once
#include <stdint.h>

// Powers the HAT, draws a 48,000-byte frame with one full refresh, hibernates, powers off.
void panel_show(const uint8_t* frame);
