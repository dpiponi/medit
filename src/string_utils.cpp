#include "string_utils.hpp"

#include <algorithm>
#include <cctype>

std::string trim_ascii_whitespace(const std::string &value) {
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
        ++start;
    }

    std::size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }

    return value.substr(start, end - start);
}

std::string ascii_lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string normalize_extension(std::string extension) {
    if (extension.empty()) {
        return extension;
    }
    if (extension[0] != '.') {
        extension.insert(extension.begin(), '.');
    }
    return ascii_lowercase(std::move(extension));
}

std::string ellipsize_middle(const std::string &text, std::size_t max_width) {
    if (text.size() <= max_width) {
        return text;
    }
    if (max_width <= 3) {
        return text.substr(0, max_width);
    }

    std::size_t prefix = (max_width - 3) / 2;
    std::size_t suffix = max_width - 3 - prefix;
    return text.substr(0, prefix) + "..." + text.substr(text.size() - suffix);
}
