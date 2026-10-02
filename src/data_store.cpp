#include "data_store.h"

#include <LittleFS.h>
#include <string.h>

static String path_of(const char* name) { return String("/") + name; }

bool store_begin() { return LittleFS.begin(true); }

bool store_load(const char* name, uint8_t* buf, size_t cap, size_t& len) {
  File f = LittleFS.open(path_of(name), "r");
  if (!f) return false;
  len = f.size();
  const bool ok = len <= cap && f.read(buf, len) == len;
  f.close();
  return ok;
}

// Write to <name>.tmp, then rename over <name>: a power cut never leaves a torn file.
bool store_save(const char* name, const uint8_t* data, size_t len) {
  const String path = path_of(name), tmp = path + ".tmp";
  File f = LittleFS.open(tmp, "w");
  if (!f) return false;
  const size_t n = f.write(data, len);
  f.close();
  if (n != len) {
    LittleFS.remove(tmp);
    return false;
  }
  if (!LittleFS.rename(tmp, path)) {  // LittleFS renames over an existing file; keep the good copy
    LittleFS.remove(tmp);
    return false;
  }
  return true;
}

void store_prune(const char* const* keep, int n) {
  String doomed[16];
  int nd = 0;
  File root = LittleFS.open("/");
  for (File f = root.openNextFile(); f && nd < 16; f = root.openNextFile()) {
    const String name = f.name();
    f.close();
    bool k = false;
    for (int i = 0; i < n; ++i) k = k || name == keep[i];
    if (!k) doomed[nd++] = name;  // collect first: removing while iterating skips entries
  }
  root.close();
  for (int i = 0; i < nd; ++i) LittleFS.remove(path_of(doomed[i].c_str()));
}
