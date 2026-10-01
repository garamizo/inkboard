#pragma once
// Streaming (SAX-style) JSON tokenizer with bounded state (spec §2.6): fed in any chunks,
// it reports each scalar with the path that leads to it, so a 370 KB FRED response never
// has to fit in RAM. Pure; host-tested with every chunk split.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <initializer_list>

namespace ink::json {

constexpr int MAX_DEPTH = 8;
constexpr size_t MAX_KEY = 31;      // longer keys are an error: no API we read has one
constexpr size_t MAX_STRING = 127;  // longer values are truncated (FRED error messages)
constexpr size_t MAX_NUMBER = 63;

enum class Type : uint8_t { String, Number, True, False, Null };

class Parser;

struct Handler {
  virtual ~Handler() = default;
  virtual void scalar(const Parser& p, Type t, const char* text) = 0;
  // A container closed; p.depth() is the depth after closing, and its key/index is still readable.
  virtual void end_container(const Parser& p) { (void)p; }
};

class Parser {
 public:
  explicit Parser(Handler& h) : h_(h) {}

  bool feed(const char* data, size_t n) {
    for (size_t i = 0; i < n && state_ != St::Error; ++i) step(data[i]);
    return state_ != St::Error;
  }

  bool finish() {
    if (state_ == St::Number && depth_ == 0) end_number();  // a bare top-level number ends at EOF
    return state_ == St::Done;
  }

  bool failed() const { return state_ == St::Error; }
  int depth() const { return depth_; }
  bool is_array(int level) const { return frames_[level].array; }
  const char* key(int level) const { return frames_[level].key; }
  int32_t index(int level) const { return frames_[level].index; }
  bool truncated() const { return truncated_; }
  // Inside end_container(): whether the container that just closed was an array.
  bool closed_array() const { return frames_[depth_].array; }

  bool at(std::initializer_list<const char*> path) const {
    if (static_cast<int>(path.size()) != depth_) return false;
    int i = 0;
    for (const char* k : path) {
      const Frame& f = frames_[i++];
      if (k == nullptr) {
        if (!f.array) return false;
      } else if (f.array || strcmp(f.key, k) != 0) {
        return false;
      }
    }
    return true;
  }

 private:
  enum class St : uint8_t { Value, FirstValueOrEnd, FirstKeyOrEnd, Key, Colon, Comma, String, Escape, Unicode,
                            Number, Literal, Done, Error };
  struct Frame {
    bool array;
    int32_t index;
    char key[MAX_KEY + 1];
  };

  static bool ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
  static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  }
  // -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
  static bool valid_number(const char* s) {
    if (*s == '-') ++s;
    if (*s == '0') ++s;
    else if (*s >= '1' && *s <= '9') while (*s >= '0' && *s <= '9') ++s;
    else return false;
    if (*s == '.') {
      ++s;
      if (*s < '0' || *s > '9') return false;
      while (*s >= '0' && *s <= '9') ++s;
    }
    if (*s == 'e' || *s == 'E') {
      ++s;
      if (*s == '+' || *s == '-') ++s;
      if (*s < '0' || *s > '9') return false;
      while (*s >= '0' && *s <= '9') ++s;
    }
    return *s == '\0';
  }

  void fail() { state_ = St::Error; }
  void after_value() { state_ = depth_ == 0 ? St::Done : St::Comma; }

  void start_string(bool is_key) {
    string_is_key_ = is_key;
    len_ = 0;
    truncated_ = false;
    hi_ = 0;
    state_ = St::String;
  }

  void value_start(char c) {
    if (ws(c)) return;
    switch (c) {
      case '{': return open(false);
      case '[': return open(true);
      case '"': return start_string(false);
      case 't': return literal("true", Type::True);
      case 'f': return literal("false", Type::False);
      case 'n': return literal("null", Type::Null);
      default:
        if (c == '-' || (c >= '0' && c <= '9')) {
          len_ = 0;
          buf_[len_++] = c;
          state_ = St::Number;
          return;
        }
        return fail();
    }
  }

  void literal(const char* word, Type t) {
    literal_ = word;
    lit_pos_ = 1;
    lit_type_ = t;
    state_ = St::Literal;
  }

  void open(bool array) {
    if (depth_ == MAX_DEPTH) return fail();
    Frame& f = frames_[depth_++];
    f.array = array;
    f.index = 0;
    f.key[0] = '\0';
    state_ = array ? St::FirstValueOrEnd : St::FirstKeyOrEnd;
  }

  void close(char c) {
    if (frames_[depth_ - 1].array != (c == ']')) return fail();
    --depth_;
    h_.end_container(*this);
    after_value();
  }

  void push(char c) {
    const size_t cap = string_is_key_ ? MAX_KEY : MAX_STRING;
    if (len_ < cap) buf_[len_++] = c;
    else if (string_is_key_) fail();
    else truncated_ = true;
  }

  void put_utf8(uint32_t cp) {
    if (cp < 0x80) return push(static_cast<char>(cp));
    if (cp < 0x800) {
      push(static_cast<char>(0xC0 | (cp >> 6)));
    } else if (cp < 0x10000) {
      push(static_cast<char>(0xE0 | (cp >> 12)));
      push(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    } else {
      push(static_cast<char>(0xF0 | (cp >> 18)));
      push(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      push(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    }
    push(static_cast<char>(0x80 | (cp & 0x3F)));
  }

  void string_char(char c) {
    if (hi_ != 0 && c != '\\') return fail();  // a high surrogate must be followed by \u low half
    if (c == '"') return end_string();
    if (c == '\\') {
      state_ = St::Escape;
      return;
    }
    if (static_cast<unsigned char>(c) < 0x20) return fail();
    push(c);
  }

  void escape_char(char c) {
    state_ = St::String;
    if (hi_ != 0 && c != 'u') return fail();
    switch (c) {
      case '"': case '\\': case '/': return push(c);
      case 'b': return push('\b');
      case 'f': return push('\f');
      case 'n': return push('\n');
      case 'r': return push('\r');
      case 't': return push('\t');
      case 'u':
        uni_ = 0;
        uni_n_ = 0;
        state_ = St::Unicode;
        return;
      default: return fail();
    }
  }

  void unicode_char(char c) {
    const int v = hexval(c);
    if (v < 0) return fail();
    uni_ = (uni_ << 4) | static_cast<uint32_t>(v);
    if (++uni_n_ < 4) return;
    state_ = St::String;
    if (uni_ >= 0xD800 && uni_ < 0xDC00) {
      if (hi_ != 0) return fail();
      hi_ = uni_;
      return;
    }
    uint32_t cp = uni_;
    if (uni_ >= 0xDC00 && uni_ < 0xE000) {
      if (hi_ == 0) return fail();
      cp = 0x10000 + ((hi_ - 0xD800) << 10) + (uni_ - 0xDC00);
    } else if (hi_ != 0) {
      return fail();
    }
    hi_ = 0;
    if (cp == 0) return fail();  // reject escaped NUL (would truncate C-strings)
    put_utf8(cp);
  }

  void end_string() {
    buf_[len_] = '\0';
    if (string_is_key_) {
      memcpy(frames_[depth_ - 1].key, buf_, len_ + 1);
      state_ = St::Colon;
      return;
    }
    h_.scalar(*this, Type::String, buf_);
    after_value();
  }

  void end_number() {
    buf_[len_] = '\0';
    if (!valid_number(buf_)) return fail();
    h_.scalar(*this, Type::Number, buf_);
    after_value();
  }

  void step(char c) {
    switch (state_) {
      case St::Value: return value_start(c);
      case St::FirstValueOrEnd:
        if (ws(c)) return;
        if (c == ']') return close(c);
        state_ = St::Value;
        return value_start(c);
      case St::FirstKeyOrEnd:
        if (ws(c)) return;
        if (c == '}') return close(c);
        if (c != '"') return fail();
        return start_string(true);
      case St::Key:
        if (ws(c)) return;
        if (c != '"') return fail();
        return start_string(true);
      case St::Colon:
        if (ws(c)) return;
        if (c != ':') return fail();
        state_ = St::Value;
        return;
      case St::Comma:
        if (ws(c)) return;
        if (c == ',') {
          Frame& f = frames_[depth_ - 1];
          if (f.array) {
            ++f.index;
            state_ = St::Value;
          } else {
            state_ = St::Key;
          }
          return;
        }
        if (c == ']' || c == '}') return close(c);
        return fail();
      case St::String: return string_char(c);
      case St::Escape: return escape_char(c);
      case St::Unicode: return unicode_char(c);
      case St::Number:
        if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') {
          if (len_ >= MAX_NUMBER) return fail();
          buf_[len_++] = c;
          return;
        }
        end_number();
        if (state_ != St::Error) step(c);  // the terminator belongs to the next state
        return;
      case St::Literal:
        if (c != literal_[lit_pos_]) return fail();
        if (literal_[++lit_pos_] == '\0') {
          h_.scalar(*this, lit_type_, literal_);
          after_value();
        }
        return;
      case St::Done:
        if (!ws(c)) fail();
        return;
      case St::Error: return;
    }
  }

  Handler& h_;
  St state_ = St::Value;
  Frame frames_[MAX_DEPTH] = {};
  int depth_ = 0;
  bool string_is_key_ = false;
  bool truncated_ = false;
  char buf_[MAX_STRING + 1] = {};
  size_t len_ = 0;
  uint32_t uni_ = 0, hi_ = 0;
  int uni_n_ = 0;
  const char* literal_ = nullptr;
  size_t lit_pos_ = 0;
  Type lit_type_ = Type::Null;
};

}  // namespace ink::json
