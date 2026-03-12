#include "json.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

JsonValue parse_json(const std::string &source) {
    try {
        return JsonValue::parse(source);
    } catch (const JsonValue::parse_error &error) {
        throw std::runtime_error(error.what());
    }
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
