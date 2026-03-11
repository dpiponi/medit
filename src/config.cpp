#include "config.hpp"
#include "json.hpp"

#include <algorithm>
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

bool parse_bool_value(const std::string &value) {
    if (value == "true") {
        return true;
    }
    if (value == "false") {
        return false;
    }
    throw std::runtime_error("invalid boolean value: " + value);
}

std::string lowercase(std::string value) {
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
    return lowercase(extension);
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

const JsonValue &required_object_member(const JsonValue &object, const char *key) {
    auto found = object.object_value.find(key);
    if (found == object.object_value.end()) {
        throw std::runtime_error(std::string("missing lsp config field: ") + key);
    }
    return found->second;
}

std::vector<LspServerConfig> load_lsp_servers_from_path(const std::filesystem::path &path) {
    std::string source = read_text_file(path);
    if (source.empty()) {
        throw std::runtime_error("could not open lsp config file: " + path.string());
    }

    JsonValue root = parse_json(source);
    if (root.type != JsonValue::Type::Object) {
        throw std::runtime_error("lsp config root must be an object");
    }

    auto servers = root.object_value.find("servers");
    if (servers == root.object_value.end() || servers->second.type != JsonValue::Type::Array) {
        throw std::runtime_error("lsp config must contain a servers array");
    }

    std::vector<LspServerConfig> parsed_servers;
    std::map<std::string, std::string> extension_owners;
    for (const JsonValue &server_value : servers->second.array_value) {
        if (server_value.type != JsonValue::Type::Object) {
            throw std::runtime_error("lsp server entries must be objects");
        }

        const JsonValue &name = required_object_member(server_value, "name");
        const JsonValue &command = required_object_member(server_value, "command");
        const JsonValue &language_id = required_object_member(server_value, "language_id");
        const JsonValue &extensions = required_object_member(server_value, "extensions");
        if (name.type != JsonValue::Type::String || command.type != JsonValue::Type::String ||
            language_id.type != JsonValue::Type::String || extensions.type != JsonValue::Type::Array) {
            throw std::runtime_error("invalid lsp server field types");
        }

        LspServerConfig server;
        server.name = name.string_value;
        server.command = command.string_value;
        server.language_id = language_id.string_value;
        for (const JsonValue &extension_value : extensions.array_value) {
            if (extension_value.type != JsonValue::Type::String) {
                throw std::runtime_error("lsp server extensions must be strings");
            }
            std::string extension = normalize_extension(extension_value.string_value);
            auto existing_owner = extension_owners.find(extension);
            if (existing_owner != extension_owners.end()) {
                throw std::runtime_error(
                    "duplicate lsp extension mapping for " + extension + ": " + existing_owner->second + " and " + name.string_value);
            }
            extension_owners.emplace(extension, name.string_value);
            server.extensions.push_back(std::move(extension));
        }
        if (server.name.empty() || server.command.empty() || server.language_id.empty() || server.extensions.empty()) {
            throw std::runtime_error("lsp server entries must not be empty");
        }
        parsed_servers.push_back(std::move(server));
    }

    return parsed_servers;
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
    config.lsp_path = first_existing_default_config_path("lsp.json");
    if (config.lsp_path && std::filesystem::exists(*config.lsp_path)) {
        config.lsp_servers = load_lsp_servers_from_path(*config.lsp_path);
    }
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
        } else if (key == "lsp") {
            config.lsp_path = resolve_config_reference(path, value);
        } else if (key == "lsp_command") {
            config.lsp_command = value;
        } else if (key == "lsp_language_id") {
            config.lsp_language_id = value;
        } else if (key == "syntax") {
            config.syntax_name = value;
        } else if (key == "right_justify_diagnostics") {
            config.right_justify_diagnostics = parse_bool_value(value);
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
    if (!config.lsp_path) {
        config.lsp_path = resolve_config_reference(path, "lsp.json");
    }
    if (config.lsp_path && std::filesystem::exists(*config.lsp_path)) {
        config.lsp_servers = load_lsp_servers_from_path(*config.lsp_path);
    } else if (config.lsp_command && config.lsp_language_id) {
        LspServerConfig fallback;
        fallback.name = *config.lsp_language_id;
        fallback.command = *config.lsp_command;
        fallback.language_id = *config.lsp_language_id;
        fallback.extensions.push_back("*");
        config.lsp_servers.push_back(std::move(fallback));
    }

    return config;
}
