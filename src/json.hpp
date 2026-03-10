#pragma once

#include <filesystem>
#include <map>
#include <string>

struct JsonValue {
    enum class Type {
        String,
        Object,
    };

    Type type = Type::String;
    std::string string_value;
    std::map<std::string, JsonValue> object_value;
};

class JsonParser {
  public:
    explicit JsonParser(std::string source);

    JsonValue parse();

  private:
    JsonValue parse_value();
    JsonValue parse_object();
    std::string parse_string();
    void skip_whitespace();
    bool peek(char expected) const;
    void expect(char expected);

    std::string source_;
    std::size_t position_ = 0;
};

JsonValue parse_json(const std::string &source);
std::string read_text_file(const std::filesystem::path &path);
