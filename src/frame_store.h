#pragma once
#include <stddef.h>
#include <stdint.h>

// Last good frame + its ETag on LittleFS (spec §6.1). Survives deep sleep and power loss.
bool store_begin();                                 // mounts, formatting on first use
bool store_has_frame();
bool store_load_frame(uint8_t* buf);                // FRAME_BYTES into buf
bool store_load_etag(char* out, size_t n);          // "" if none
bool store_save(const uint8_t* buf, const char* etag);
void store_clear_etag();
