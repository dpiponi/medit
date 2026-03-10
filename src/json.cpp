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
