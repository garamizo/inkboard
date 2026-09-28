#pragma once
#include <stdint.h>

#include "cycle.h"

bool wifi_connect(uint32_t timeout_ms);
void wifi_off();
// GET SERVER_URL/v1/frame.bin?FRAME_QUERY (or the calibration pattern); the body goes into buf.
wake::Fetched fetch_frame(const char* etag, uint8_t* buf);
