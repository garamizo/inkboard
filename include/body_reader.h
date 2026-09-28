#pragma once
// Reads an HTTP body that must be exactly `expect` bytes (spec §2.3: 48,000). Generic
// over the stream and clock so it runs on the host in tests.
#include <stddef.h>
#include <stdint.h>

namespace body {

// Returns `expect` for a complete body of exactly that size, else -1.
// content_length: the header's value, or -1 when absent (body ends at connection close).
template <class Stream, class Now, class Idle>
int32_t read_exact(Stream& s, uint8_t* buf, int32_t expect, int32_t content_length, uint32_t timeout_ms,
                   Now now_ms, Idle idle) {
  if (content_length >= 0 && content_length != expect) return -1;
  const uint32_t start = now_ms();
  int32_t got = 0;
  while (got < expect) {
    if (now_ms() - start > timeout_ms) return -1;
    int avail = s.available();
    if (avail > 0) {
      int32_t want = expect - got < avail ? expect - got : avail;
      int n = s.read(buf + got, static_cast<size_t>(want));
      if (n <= 0) return -1;
      got += n;
    } else if (!s.connected()) {
      return -1;  // closed early: truncated
    } else {
      idle();
    }
  }
  if (content_length == expect) return got;  // the declared length says the body is done
  // No declared length: the body ends only when the server closes, and nothing may follow.
  while (now_ms() - start <= timeout_ms) {
    if (s.available() > 0) return -1;
    if (!s.connected()) return got;
    idle();
  }
  return -1;
}

}  // namespace body
