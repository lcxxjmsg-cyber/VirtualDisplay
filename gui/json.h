#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace vdgui {

/**
 * @brief Minimal JSON value used to parse iddctrl --json output.
 *
 * Supports objects, arrays, strings, numbers, booleans and null.
 * Designed for our own control-tool output; not a general-purpose parser.
 */
class JsonValue {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  Type type = Type::Null;
  bool boolValue = false;
  double numberValue = 0.0;
  std::string stringValue;
  std::vector<JsonValue> arrayValue;
  std::map<std::string, JsonValue> objectValue;

  bool IsObject() const { return type == Type::Object; }
  bool IsArray() const { return type == Type::Array; }
  bool IsString() const { return type == Type::String; }
  bool IsNumber() const { return type == Type::Number; }
  bool IsBool() const { return type == Type::Bool; }
  bool IsNull() const { return type == Type::Null; }

  const JsonValue &Get(const std::string &key) const {
    static const JsonValue kNull;
    auto it = objectValue.find(key);
    if (it == objectValue.end()) return kNull;
    return it->second;
  }

  std::string Str(const std::string &key, const std::string &dflt = "") const {
    const JsonValue &v = Get(key);
    if (v.IsString()) return v.stringValue;
    if (v.IsNumber()) return std::to_string(static_cast<long long>(v.numberValue));
    return dflt;
  }

  long long Int(const std::string &key, long long dflt = 0) const {
    const JsonValue &v = Get(key);
    if (v.IsNumber()) return static_cast<long long>(v.numberValue);
    if (v.IsBool()) return v.boolValue ? 1 : 0;
    return dflt;
  }

  bool Bool(const std::string &key, bool dflt = false) const {
    const JsonValue &v = Get(key);
    if (v.IsBool()) return v.boolValue;
    if (v.IsNumber()) return v.numberValue != 0;
    return dflt;
  }
};

/**
 * @brief Parse a JSON document from a string.
 *
 * @param text Input JSON.
 * @param ok Receives parse success.
 * @return Parsed root value (Null on failure).
 */
JsonValue JsonParse(const std::string &text, bool *ok = nullptr);

}  // namespace vdgui
