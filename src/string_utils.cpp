#include "string_utils.hpp"

#include <algorithm>
#include <cctype>

std::string trim_ascii_whitespace(std::string_view value) {
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
        ++start;
    }

    std::size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }

    return std::string(value.substr(start, end - start));
}

std::string ascii_lowercase(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

std::string normalize_extension(std::string_view extension) {
    if (extension.empty()) {
        return std::string(extension);
    }
    if (extension[0] == '.') {
        return ascii_lowercase(extension);
    }
    return "." + ascii_lowercase(extension);
}

std::string ellipsize_middle(std::string_view text, std::size_t max_width) {
    if (text.size() <= max_width) {
        return std::string(text);
    }
    if (max_width <= 3) {
        return std::string(text.substr(0, max_width));
    }

    std::size_t prefix = (max_width - 3) / 2;
    std::size_t suffix = max_width - 3 - prefix;
    return std::string(text.substr(0, prefix)) + "..." + std::string(text.substr(text.size() - suffix));
}
