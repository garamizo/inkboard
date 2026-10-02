#pragma once
#include <stdint.h>

#include "data_cache.h"
#include "json_stream.h"

bool net_wifi_up(uint32_t timeout_ms);
void net_wifi_off();                    // also closes the keep-alive connection
bool net_sntp(uint32_t timeout_ms);     // true once the system clock is set
void net_set_clock(int64_t epoch);
// HTTPS GET with the body streamed into a JSON handler; one keep-alive connection per host.
ink::FetchResult net_get(const char* host, const char* path, ink::json::Handler& h, uint32_t timeout_ms);
void net_close();
