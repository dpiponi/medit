#include "json.hpp"

#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

JsonParser::JsonParser(std::string source) : source_(std::move(source)) {}

JsonValue JsonParser::parse() {
    skip_whitespace();
    JsonValue value = parse_value();
    skip_whitespace();
    if (position_ != source_.size()) {
        throw std::runtime_error("unexpected trailing JSON content");
    }
    return value;
}

JsonValue JsonParser::parse_value() {
    skip_whitespace();
    if (position_ >= source_.size()) {
        throw std::runtime_error("unexpected end of JSON");
    }
    if (source_[position_] == '"') {
        JsonValue value;
        value.type = JsonValue::Type::String;
        value.string_value = parse_string();
        return value;
    }
    if (source_[position_] == '{') {
        return parse_object();
    }
    if (source_[position_] == '[') {
        return parse_array();
    }
    if (source_[position_] == '-' || std::isdigit(static_cast<unsigned char>(source_[position_]))) {
        return parse_number();
    }
    if (source_[position_] == 't') {
        return parse_true();
    }
    if (source_[position_] == 'f') {
        return parse_false();
    }
    if (source_[position_] == 'n') {
        return parse_null();
    }
    throw std::runtime_error("unsupported JSON value");
}

JsonValue JsonParser::parse_object() {
    expect('{');
    JsonValue value;
    value.type = JsonValue::Type::Object;
    skip_whitespace();
    if (peek('}')) {
        expect('}');
        return value;
    }
    while (true) {
        skip_whitespace();
        std::string key = parse_string();
        skip_whitespace();
        expect(':');
        skip_whitespace();
        value.object_value.emplace(key, parse_value());
        skip_whitespace();
        if (peek('}')) {
            expect('}');
            break;
        }
        expect(',');
    }
    return value;
}

JsonValue JsonParser::parse_array() {
    expect('[');
    JsonValue value;
    value.type = JsonValue::Type::Array;
    skip_whitespace();
    if (peek(']')) {
        expect(']');
        return value;
    }
    while (true) {
        value.array_value.push_back(parse_value());
        skip_whitespace();
        if (peek(']')) {
            expect(']');
            break;
        }
        expect(',');
    }
    return value;
}

JsonValue JsonParser::parse_number() {
    std::size_t start = position_;
    if (source_[position_] == '-') {
        ++position_;
    }
    while (position_ < source_.size() && std::isdigit(static_cast<unsigned char>(source_[position_]))) {
        ++position_;
    }
    if (position_ < source_.size() && source_[position_] == '.') {
        ++position_;
        while (position_ < source_.size() && std::isdigit(static_cast<unsigned char>(source_[position_]))) {
            ++position_;
        }
    }
    if (position_ < source_.size() && (source_[position_] == 'e' || source_[position_] == 'E')) {
        ++position_;
        if (position_ < source_.size() && (source_[position_] == '+' || source_[position_] == '-')) {
            ++position_;
        }
        while (position_ < source_.size() && std::isdigit(static_cast<unsigned char>(source_[position_]))) {
            ++position_;
        }
    }
    JsonValue value;
    value.type = JsonValue::Type::Number;
    value.number_value = std::stod(source_.substr(start, position_ - start));
    return value;
}

JsonValue JsonParser::parse_true() {
    if (!match_literal("true")) {
        throw std::runtime_error("unexpected JSON token");
    }
    JsonValue value;
    value.type = JsonValue::Type::Bool;
    value.bool_value = true;
    return value;
}

JsonValue JsonParser::parse_false() {
    if (!match_literal("false")) {
        throw std::runtime_error("unexpected JSON token");
    }
    JsonValue value;
    value.type = JsonValue::Type::Bool;
    value.bool_value = false;
    return value;
}

JsonValue JsonParser::parse_null() {
    if (!match_literal("null")) {
        throw std::runtime_error("unexpected JSON token");
    }
    JsonValue value;
    value.type = JsonValue::Type::Null;
    return value;
}

std::string JsonParser::parse_string() {
    expect('"');
    std::string result;
    while (position_ < source_.size()) {
        char ch = source_[position_++];
        if (ch == '"') {
            return result;
        }
        if (ch == '\\') {
            if (position_ >= source_.size()) {
                throw std::runtime_error("unterminated JSON escape");
            }
            char escaped = source_[position_++];
            switch (escaped) {
                case '"':
                case '\\':
                case '/':
                    result.push_back(escaped);
                    break;
                case 'b':
                    result.push_back('\b');
                    break;
                case 'f':
                    result.push_back('\f');
                    break;
                case 'n':
                    result.push_back('\n');
                    break;
                case 'r':
                    result.push_back('\r');
                    break;
                case 't':
                    result.push_back('\t');
                    break;
                default:
                    throw std::runtime_error("unsupported JSON escape");
            }
            continue;
        }
        result.push_back(ch);
    }
    throw std::runtime_error("unterminated JSON string");
}

void JsonParser::skip_whitespace() {
    while (position_ < source_.size() && std::isspace(static_cast<unsigned char>(source_[position_]))) {
        ++position_;
    }
}

bool JsonParser::peek(char expected) const {
    return position_ < source_.size() && source_[position_] == expected;
}

void JsonParser::expect(char expected) {
    if (position_ >= source_.size() || source_[position_] != expected) {
        throw std::runtime_error("unexpected JSON token");
    }
    ++position_;
}

bool JsonParser::match_literal(const std::string &literal) {
    if (source_.compare(position_, literal.size(), literal) != 0) {
        return false;
    }
    position_ += literal.size();
    return true;
}

JsonValue parse_json(const std::string &source) {
    return JsonParser(source).parse();
}

std::string read_text_file(const std::filesystem::path &path) {
    std::ifstream input(path);
    if (!input) {
        return "";
    }
    std::stringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}
