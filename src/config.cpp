#include "config.hpp"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

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

std::optional<std::filesystem::path> first_existing_meditrc_path() {
    std::filesystem::path local = std::filesystem::current_path() / ".config" / "meditrc";
    if (std::filesystem::exists(local)) {
        return local;
    }

    const char *home = std::getenv("HOME");
    if (home != nullptr) {
        std::filesystem::path global = std::filesystem::path(home) / ".config" / "meditrc";
        if (std::filesystem::exists(global)) {
            return global;
        }
    }

    return std::nullopt;
}

std::optional<std::filesystem::path> first_existing_default_config_path(const std::string &file_name) {
    std::filesystem::path local = std::filesystem::current_path() / ".config" / "medit" / file_name;
    if (std::filesystem::exists(local)) {
        return local;
    }

    const char *home = std::getenv("HOME");
    if (home != nullptr) {
        std::filesystem::path global = std::filesystem::path(home) / ".config" / "medit" / file_name;
        if (std::filesystem::exists(global)) {
            return global;
        }
    }

    return std::nullopt;
}

std::filesystem::path resolve_config_reference(
    const std::filesystem::path &meditrc_path,
    const std::string &value) {
    std::filesystem::path path = value;
    if (path.is_absolute()) {
        return path;
    }
    return meditrc_path.parent_path() / "medit" / path;
}

}  // namespace

EditorConfig load_editor_config() {
    std::optional<std::filesystem::path> rc_path = first_existing_meditrc_path();
    if (rc_path) {
        return load_editor_config_from_path(*rc_path);
    }

    EditorConfig config;
    config.keybindings_path = first_existing_default_config_path("keybindings.json");
    config.colors_path = first_existing_default_config_path("colors.json");
    return config;
}

EditorConfig load_editor_config_from_path(const std::filesystem::path &path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("could not open config file: " + path.string());
    }

    EditorConfig config;
    config.source_path = path.string();

    std::string line;
    while (std::getline(input, line)) {
        std::string content = trim(line);
        if (content.empty() || content[0] == '#') {
            continue;
        }
        std::size_t separator = content.find('=');
        if (separator == std::string::npos) {
            throw std::runtime_error("invalid config line: " + content);
        }

        std::string key = trim(content.substr(0, separator));
        std::string value = trim(content.substr(separator + 1));
        if (key.empty() || value.empty()) {
            throw std::runtime_error("invalid config line: " + content);
        }

        if (key == "keybindings") {
            config.keybindings_path = resolve_config_reference(path, value);
        } else if (key == "colors") {
            config.colors_path = resolve_config_reference(path, value);
        } else if (key == "lsp_command") {
            config.lsp_command = value;
        } else if (key == "lsp_language_id") {
            config.lsp_language_id = value;
        } else {
            throw std::runtime_error("unknown config key: " + key);
        }
    }

    if (!config.keybindings_path) {
        config.keybindings_path = resolve_config_reference(path, "keybindings.json");
    }
    if (!config.colors_path) {
        config.colors_path = resolve_config_reference(path, "colors.json");
    }

    return config;
}
