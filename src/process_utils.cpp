#include "process_utils.hpp"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

std::string trim(const std::string &value) {
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

std::vector<std::string> split_shell_pipeline(const std::string &pipeline) {
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
            segments.push_back(trim(current));
            current.clear();
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty()) {
        segments.push_back(trim(current));
    }
    return segments;
}

bool executable_exists(const std::string &executable) {
    if (executable.empty()) {
        return false;
    }
    std::filesystem::path path = executable;
    if (path.is_absolute() || executable.find('/') != std::string::npos) {
#if defined(__unix__) || defined(__APPLE__)
        return access(executable.c_str(), X_OK) == 0;
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
        std::filesystem::path candidate = directory.empty() ? std::filesystem::path(executable) : std::filesystem::path(directory) / executable;
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

std::optional<std::string> first_command_word(const std::string &command) {
    std::string trimmed = trim(command);
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

std::optional<std::string> missing_executable_in_command(const std::string &command) {
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

std::optional<std::string> missing_executable_in_pipeline(const std::string &pipeline) {
    for (const std::string &segment : split_shell_pipeline(pipeline)) {
        if (std::optional<std::string> missing = missing_executable_in_command(segment)) {
            return missing;
        }
    }
    return std::nullopt;
}
