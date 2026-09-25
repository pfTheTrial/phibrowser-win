// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_TEST_MODEL_JSON_LITE_H_
#define PHIBROWSER_WIN_TEST_MODEL_JSON_LITE_H_

// A small JSON reader for host tests that replay data files (the sync
// conformance vectors). The host tests are STL-only, so base::JSONReader is
// out of reach. Strict enough for machine-written files: objects, arrays,
// strings with \uXXXX escapes (surrogate pairs included), numbers kept as
// their source text (the vectors carry 64-bit values as strings anyway),
// true/false/null. A syntax error throws std::runtime_error.

#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace json_lite {

struct Value {
  enum class Kind { kNull, kBool, kNumber, kString, kArray, kObject };
  Kind kind = Kind::kNull;
  bool boolean = false;
  std::string text;  // kString contents, or kNumber's source text.
  std::vector<Value> array;
  std::map<std::string, Value> object;

  bool is_null() const { return kind == Kind::kNull; }
  const Value& operator[](const std::string& key) const {
    static const Value kNull;
    const auto it = object.find(key);
    return it == object.end() ? kNull : it->second;
  }
  const std::string& str() const {
    if (kind != Kind::kString) {
      throw std::runtime_error("json_lite: not a string");
    }
    return text;
  }
  int64_t i64() const {
    // 64-bit values travel as decimal strings; plain numbers are accepted too.
    if (kind != Kind::kString && kind != Kind::kNumber) {
      throw std::runtime_error("json_lite: not an integer");
    }
    return std::stoll(text);
  }
};

class Parser {
 public:
  explicit Parser(const std::string& text) : s_(text) {}

  Value Parse() {
    Value v = ParseValue();
    SkipSpace();
    if (at_ != s_.size()) {
      Fail("trailing data");
    }
    return v;
  }

 private:
  [[noreturn]] void Fail(const char* what) {
    throw std::runtime_error(std::string("json_lite: ") + what + " at " +
                             std::to_string(at_));
  }
  void SkipSpace() {
    while (at_ < s_.size() && (s_[at_] == ' ' || s_[at_] == '\n' ||
                               s_[at_] == '\r' || s_[at_] == '\t')) {
      ++at_;
    }
  }
  char Peek() {
    SkipSpace();
    if (at_ >= s_.size()) {
      Fail("unexpected end");
    }
    return s_[at_];
  }
  void Expect(const char* word) {
    for (const char* p = word; *p; ++p) {
      if (at_ >= s_.size() || s_[at_] != *p) {
        Fail("bad literal");
      }
      ++at_;
    }
  }
  static void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }
  uint32_t Hex4() {
    if (at_ + 4 > s_.size()) {
      Fail("short \\u escape");
    }
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = s_[at_++];
      v <<= 4;
      if (c >= '0' && c <= '9') {
        v |= static_cast<uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        v |= static_cast<uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        v |= static_cast<uint32_t>(c - 'A' + 10);
      } else {
        Fail("bad \\u escape");
      }
    }
    return v;
  }
  std::string ParseString() {
    if (Peek() != '"') {
      Fail("expected string");
    }
    ++at_;
    std::string out;
    while (true) {
      if (at_ >= s_.size()) {
        Fail("unterminated string");
      }
      const char c = s_[at_++];
      if (c == '"') {
        return out;
      }
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (at_ >= s_.size()) {
        Fail("bad escape");
      }
      const char e = s_[at_++];
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          uint32_t cp = Hex4();
          if (cp >= 0xD800 && cp <= 0xDBFF && at_ + 6 <= s_.size() &&
              s_[at_] == '\\' && s_[at_ + 1] == 'u') {
            at_ += 2;
            const uint32_t low = Hex4();
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          }
          AppendUtf8(out, cp);
          break;
        }
        default:
          Fail("bad escape");
      }
    }
  }
  Value ParseValue() {
    Value v;
    const char c = Peek();
    if (c == '{') {
      ++at_;
      v.kind = Value::Kind::kObject;
      if (Peek() == '}') {
        ++at_;
        return v;
      }
      while (true) {
        std::string key = ParseString();
        if (Peek() != ':') {
          Fail("expected ':'");
        }
        ++at_;
        v.object[key] = ParseValue();
        const char next = Peek();
        ++at_;
        if (next == '}') {
          return v;
        }
        if (next != ',') {
          Fail("expected ',' or '}'");
        }
      }
    }
    if (c == '[') {
      ++at_;
      v.kind = Value::Kind::kArray;
      if (Peek() == ']') {
        ++at_;
        return v;
      }
      while (true) {
        v.array.push_back(ParseValue());
        const char next = Peek();
        ++at_;
        if (next == ']') {
          return v;
        }
        if (next != ',') {
          Fail("expected ',' or ']'");
        }
      }
    }
    if (c == '"') {
      v.kind = Value::Kind::kString;
      v.text = ParseString();
      return v;
    }
    if (c == 't') {
      Expect("true");
      v.kind = Value::Kind::kBool;
      v.boolean = true;
      return v;
    }
    if (c == 'f') {
      Expect("false");
      v.kind = Value::Kind::kBool;
      return v;
    }
    if (c == 'n') {
      Expect("null");
      return v;
    }
    const size_t start = at_;
    while (at_ < s_.size() &&
           (s_[at_] == '-' || s_[at_] == '+' || s_[at_] == '.' ||
            s_[at_] == 'e' || s_[at_] == 'E' ||
            (s_[at_] >= '0' && s_[at_] <= '9'))) {
      ++at_;
    }
    if (start == at_) {
      Fail("unexpected character");
    }
    v.kind = Value::Kind::kNumber;
    v.text = s_.substr(start, at_ - start);
    return v;
  }

  const std::string& s_;
  size_t at_ = 0;
};

inline Value ParseFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("json_lite: cannot open " + path);
  }
  std::stringstream buffer;
  buffer << in.rdbuf();
  const std::string text = buffer.str();
  return Parser(text).Parse();
}

}  // namespace json_lite

#endif  // PHIBROWSER_WIN_TEST_MODEL_JSON_LITE_H_
