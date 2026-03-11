#include "process_utils.hpp"
#include "string_utils.hpp"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

std::vector<std::string> split_shell_pipeline(std::string_view pipeline) {
    std::vector<std::string> segments;
    std::string current;
    bool in_single_quote = false;
    bool in_double_quote = false;
    bool escaped = false;
    for (char ch : pipeline) {
        if (escaped) {
            current.push_back(ch);
            escaped = false;
            continue;
        }
        if (ch == '\\' && !in_single_quote) {
            escaped = true;
            continue;
        }
        if (ch == '\'' && !in_double_quote) {
            in_single_quote = !in_single_quote;
            continue;
        }
        if (ch == '"' && !in_single_quote) {
            in_double_quote = !in_double_quote;
            continue;
        }
        if (ch == '|' && !in_single_quote && !in_double_quote) {
            segments.push_back(trim_ascii_whitespace(current));
            current.clear();
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty()) {
        segments.push_back(trim_ascii_whitespace(current));
    }
    return segments;
}

bool executable_exists(std::string_view executable) {
    if (executable.empty()) {
        return false;
    }
    std::string executable_text(executable);
    std::filesystem::path path = executable_text;
    if (path.is_absolute() || executable.contains('/')) {
#if defined(__unix__) || defined(__APPLE__)
        return access(executable_text.c_str(), X_OK) == 0;
#else
        return std::filesystem::exists(path);
#endif
    }

    const char *path_env = std::getenv("PATH");
    if (path_env == nullptr) {
        return false;
    }
    std::string search_path = path_env;
    std::size_t start = 0;
    while (start <= search_path.size()) {
        std::size_t end = search_path.find(':', start);
        std::string directory = end == std::string::npos ? search_path.substr(start) : search_path.substr(start, end - start);
        std::filesystem::path candidate =
            directory.empty() ? std::filesystem::path(executable_text) : std::filesystem::path(directory) / executable_text;
#if defined(__unix__) || defined(__APPLE__)
        if (access(candidate.c_str(), X_OK) == 0) {
            return true;
        }
#else
        if (std::filesystem::exists(candidate)) {
            return true;
        }
#endif
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return false;
}

std::optional<std::string> first_command_word(std::string_view command) {
    std::string trimmed = trim_ascii_whitespace(command);
    if (trimmed.empty()) {
        return std::nullopt;
    }

    std::string word;
    bool in_single_quote = false;
    bool in_double_quote = false;
    bool escaped = false;
    for (char ch : trimmed) {
        if (escaped) {
            word.push_back(ch);
            escaped = false;
            continue;
        }
        if (ch == '\\' && !in_single_quote) {
            escaped = true;
            continue;
        }
        if (ch == '\'' && !in_double_quote) {
            in_single_quote = !in_single_quote;
            continue;
        }
        if (ch == '"' && !in_single_quote) {
            in_double_quote = !in_double_quote;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(ch)) && !in_single_quote && !in_double_quote) {
            break;
        }
        word.push_back(ch);
    }

    if (word.empty()) {
        return std::nullopt;
    }
    return word;
}

std::optional<std::string> missing_executable_in_command(std::string_view command) {
    std::optional<std::string> word = first_command_word(command);
    if (!word) {
        return std::nullopt;
    }
    if (executable_exists(*word)) {
        return std::nullopt;
    }
    return word;
}

std::optional<std::string> first_available_executable(const std::vector<std::string> &executables) {
    for (const std::string &executable : executables) {
        if (executable_exists(executable)) {
            return executable;
        }
    }
    return std::nullopt;
}

std::optional<std::string> missing_executable_in_pipeline(std::string_view pipeline) {
    for (const std::string &segment : split_shell_pipeline(pipeline)) {
        if (std::optional<std::string> missing = missing_executable_in_command(segment)) {
            return missing;
        }
    }
    return std::nullopt;
}
