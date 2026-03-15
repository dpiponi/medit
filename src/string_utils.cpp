#include "string_utils.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>

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

std::string expand_user_path(std::string_view path) {
    if (path.empty() || path[0] != '~') {
        return std::string(path);
    }
    if (path.size() > 1 && path[1] != '/') {
        return std::string(path);
    }
    const char *home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') {
        return std::string(path);
    }
    if (path.size() == 1) {
        return std::string(home);
    }
    return std::string(home) + std::string(path.substr(1));
}

bool glob_match(std::string_view text, std::string_view pattern) {
    std::size_t text_index = 0;
    std::size_t pattern_index = 0;
    std::size_t star_index = std::string_view::npos;
    std::size_t match_index = 0;

    while (text_index < text.size()) {
        if (pattern_index < pattern.size() &&
            (pattern[pattern_index] == '?' || pattern[pattern_index] == text[text_index])) {
            ++text_index;
            ++pattern_index;
            continue;
        }
        if (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
            star_index = pattern_index++;
            match_index = text_index;
            continue;
        }
        if (star_index != std::string_view::npos) {
            pattern_index = star_index + 1;
            text_index = ++match_index;
            continue;
        }
        return false;
    }

    while (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
        ++pattern_index;
    }
    return pattern_index == pattern.size();
}

bool file_path_matches_glob(std::string_view file_path, std::string_view pattern) {
    if (pattern.empty()) {
        return false;
    }
    std::string file_path_text(file_path);
    std::string pattern_text(pattern);
    if (pattern_text.contains('/') || pattern_text.contains('\\')) {
        return glob_match(file_path_text, pattern_text);
    }
    return glob_match(std::filesystem::path(file_path_text).filename().string(), pattern_text);
}
