#pragma once
#include <stddef.h>
#include <stdint.h>

// Cache files on LittleFS (spec §2.5): names are bare ("weather.bin"), stored at the root.
bool store_begin();  // mounts, formatting on failure: the partition only holds caches
bool store_load(const char* name, uint8_t* buf, size_t cap, size_t& len);
bool store_save(const char* name, const uint8_t* data, size_t len);  // temp file + rename
void store_prune(const char* const* keep, int n);                    // deletes every other file
