#include "json.h"

namespace vdgui {

namespace {

class Parser {
 public:
  explicit Parser(const std::string &text) : text_(text) {}

  JsonValue ParseValue() {
    SkipWs();
    if (pos_ >= text_.size()) return JsonValue();
    switch (text_[pos_]) {
      case '{': return ParseObject();
      case '[': return ParseArray();
      case '"': return ParseString();
      case 't': return ParseLiteral("true", true);
      case 'f': return ParseLiteral("false", false);
      case 'n': return ParseLiteral("null", false);
      default: return ParseNumber();
    }
  }

 private:
  const std::string &text_;
  size_t pos_ = 0;

  void SkipWs() {
    while (pos_ < text_.size() &&
           (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\r' || text_[pos_] == '\n')) {
      pos_++;
    }
  }

  JsonValue ParseObject() {
    JsonValue v;
    v.type = JsonValue::Type::Object;
    pos_++;  // '{'
    SkipWs();
    if (pos_ < text_.size() && text_[pos_] == '}') {
      pos_++;
      return v;
    }
    while (pos_ < text_.size()) {
      SkipWs();
      if (pos_ >= text_.size() || text_[pos_] != '"') break;
      JsonValue key = ParseString();
      SkipWs();
      if (pos_ >= text_.size() || text_[pos_] != ':') break;
      pos_++;
      JsonValue val = ParseValue();
      v.objectValue[key.stringValue] = std::move(val);
      SkipWs();
      if (pos_ < text_.size() && text_[pos_] == ',') {
        pos_++;
        continue;
      }
      if (pos_ < text_.size() && text_[pos_] == '}') {
        pos_++;
        break;
      }
      break;
    }
    return v;
  }

  JsonValue ParseArray() {
    JsonValue v;
    v.type = JsonValue::Type::Array;
    pos_++;  // '['
    SkipWs();
    if (pos_ < text_.size() && text_[pos_] == ']') {
      pos_++;
      return v;
    }
    while (pos_ < text_.size()) {
      JsonValue val = ParseValue();
      v.arrayValue.push_back(std::move(val));
      SkipWs();
      if (pos_ < text_.size() && text_[pos_] == ',') {
        pos_++;
        continue;
      }
      if (pos_ < text_.size() && text_[pos_] == ']') {
        pos_++;
        break;
      }
      break;
    }
    return v;
  }

  JsonValue ParseString() {
    JsonValue v;
    v.type = JsonValue::Type::String;
    pos_++;  // '"'
    std::string out;
    while (pos_ < text_.size()) {
      char c = text_[pos_++];
      if (c == '"') break;
      if (c == '\\' && pos_ < text_.size()) {
        char e = text_[pos_++];
        switch (e) {
          case '"': out += '"'; break;
          case '\\': out += '\\'; break;
          case '/': out += '/'; break;
          case 'n': out += '\n'; break;
          case 'r': out += '\r'; break;
          case 't': out += '\t'; break;
          case 'u': {
            // Best-effort: skip 4 hex digits (non-ASCII unlikely in our output)
            for (int i = 0; i < 4 && pos_ < text_.size(); i++) pos_++;
            break;
          }
          default: out += e; break;
        }
      } else {
        out += c;
      }
    }
    v.stringValue = std::move(out);
    return v;
  }

  JsonValue ParseLiteral(const char *lit, bool value) {
    JsonValue v;
    v.type = value ? JsonValue::Type::Bool : JsonValue::Type::Null;
    if (value) v.boolValue = true;
    size_t len = strlen(lit);
    if (pos_ + len <= text_.size() && text_.compare(pos_, len, lit) == 0) {
      pos_ += len;
    }
    return v;
  }

  JsonValue ParseNumber() {
    JsonValue v;
    v.type = JsonValue::Type::Number;
    size_t start = pos_;
    while (pos_ < text_.size()) {
      char c = text_[pos_];
      if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') {
        pos_++;
      } else {
        break;
      }
    }
    v.numberValue = atof(text_.substr(start, pos_ - start).c_str());
    return v;
  }
};

}  // namespace

JsonValue JsonParse(const std::string &text, bool *ok) {
  Parser p(text);
  JsonValue v = p.ParseValue();
  if (ok) {
    *ok = !v.IsNull();
  }
  return v;
}

}  // namespace vdgui
