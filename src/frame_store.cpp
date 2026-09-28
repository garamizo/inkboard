#include "frame_store.h"

#include <LittleFS.h>
#include <string.h>

#include "wake_logic.h"

static const char* kFrame = "/frame.bin";
static const char* kEtag = "/etag.txt";

// Write to <path>.tmp, then rename over <path>: a power cut never leaves a torn file.
static bool write_atomic(const char* path, const uint8_t* data, size_t len) {
  String tmp = String(path) + ".tmp";
  File f = LittleFS.open(tmp, "w");
  if (!f) return false;
  size_t n = f.write(data, len);
  f.close();
  if (n != len) {
    LittleFS.remove(tmp);
    return false;
  }
  if (!LittleFS.rename(tmp, path)) {
    LittleFS.remove(path);
    if (!LittleFS.rename(tmp, path)) return false;
  }
  return true;
}

bool store_begin() { return LittleFS.begin(true); }

bool store_has_frame() {
  File f = LittleFS.open(kFrame, "r");
  bool ok = f && f.size() == static_cast<size_t>(wake::FRAME_BYTES);
  if (f) f.close();
  return ok;
}

bool store_load_frame(uint8_t* buf) {
  File f = LittleFS.open(kFrame, "r");
  if (!f) return false;
  size_t n = f.read(buf, wake::FRAME_BYTES);
  f.close();
  return n == static_cast<size_t>(wake::FRAME_BYTES);
}

bool store_load_etag(char* out, size_t n) {
  out[0] = '\0';
  File f = LittleFS.open(kEtag, "r");
  if (!f) return false;
  size_t got = f.readBytes(out, n - 1);
  f.close();
  out[got] = '\0';
  return got > 0;
}

bool store_save(const uint8_t* buf, const char* etag) {
  // Frame first: a stored ETag must never describe a frame that isn't there.
  if (!write_atomic(kFrame, buf, wake::FRAME_BYTES)) return false;
  return write_atomic(kEtag, reinterpret_cast<const uint8_t*>(etag), strlen(etag));
}

void store_clear_etag() { LittleFS.remove(kEtag); }
