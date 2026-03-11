#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

struct JsonValue {
    enum class Type {
        String,
        Object,
        Array,
        Number,
        Bool,
        Null,
    };

    Type type = Type::String;
    std::string string_value;
    std::map<std::string, JsonValue> object_value;
    std::vector<JsonValue> array_value;
    double number_value = 0.0;
    bool bool_value = false;
};

class JsonParser {
  public:
    explicit JsonParser(std::string source);

    JsonValue parse();

  private:
    JsonValue parse_value();
    JsonValue parse_object();
    JsonValue parse_array();
    JsonValue parse_number();
    JsonValue parse_true();
    JsonValue parse_false();
    JsonValue parse_null();
    std::string parse_string();
    bool at_end() const;
    char current() const;
    char advance();
    void skip_whitespace();
    bool peek(char expected) const;
    void expect(char expected);
    bool match_literal(const std::string &literal);

    std::string source_;
    std::size_t position_ = 0;
};

JsonValue parse_json(const std::string &source);
std::string read_text_file(const std::filesystem::path &path);
