#include "config.hpp"
#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>

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

LspServerConfig::WorkspaceConfig parse_workspace_config(const JsonValue &value) {
    if (value.type != JsonValue::Type::Object) {
        throw std::runtime_error("lsp workspace config must be an object");
    }

    LspServerConfig::WorkspaceConfig workspace;
    auto markers = value.object_value.find("markers");
    if (markers != value.object_value.end()) {
        if (markers->second.type != JsonValue::Type::Array) {
            throw std::runtime_error("lsp workspace markers must be an array");
        }
        for (const JsonValue &marker_value : markers->second.array_value) {
            if (marker_value.type != JsonValue::Type::String) {
                throw std::runtime_error("lsp workspace markers must be strings");
            }
            workspace.markers.push_back(marker_value.string_value);
        }
    }

    auto fallback = value.object_value.find("fallback");
    if (fallback != value.object_value.end()) {
        if (fallback->second.type != JsonValue::Type::String) {
            throw std::runtime_error("lsp workspace fallback must be a string");
        }
        workspace.fallback = fallback->second.string_value;
    }

    if (workspace.fallback != "file_directory" && workspace.fallback != "current_working_directory") {
        throw std::runtime_error("unsupported lsp workspace fallback: " + workspace.fallback);
    }
    return workspace;
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
        auto workspace = server_value.object_value.find("workspace");
        if (workspace != server_value.object_value.end()) {
            server.workspace = parse_workspace_config(workspace->second);
        }
        parsed_servers.push_back(std::move(server));
    }

    return parsed_servers;
}

std::vector<SyntaxLanguageConfig> load_syntax_languages_from_path(const std::filesystem::path &path) {
    std::string source = read_text_file(path);
    if (source.empty()) {
        throw std::runtime_error("could not open syntax config file: " + path.string());
    }

    JsonValue root = parse_json(source);
    if (root.type != JsonValue::Type::Object) {
        throw std::runtime_error("syntax config root must be an object");
    }

    auto languages = root.object_value.find("languages");
    if (languages == root.object_value.end() || languages->second.type != JsonValue::Type::Array) {
        throw std::runtime_error("syntax config must contain a languages array");
    }

    std::vector<SyntaxLanguageConfig> parsed_languages;
    std::map<std::string, std::string> extension_owners;
    for (const JsonValue &language_value : languages->second.array_value) {
        if (language_value.type != JsonValue::Type::Object) {
            throw std::runtime_error("syntax language entries must be objects");
        }

        const JsonValue &name = required_object_member(language_value, "name");
        const JsonValue &extensions = required_object_member(language_value, "extensions");
        const JsonValue &grammar_path = required_object_member(language_value, "grammar_path");
        const JsonValue &symbol_name = required_object_member(language_value, "symbol_name");
        const JsonValue &highlights_path = required_object_member(language_value, "highlights_path");
        if (name.type != JsonValue::Type::String || extensions.type != JsonValue::Type::Array ||
            grammar_path.type != JsonValue::Type::String || symbol_name.type != JsonValue::Type::String ||
            highlights_path.type != JsonValue::Type::String) {
            throw std::runtime_error("invalid syntax language field types");
        }

        SyntaxLanguageConfig language;
        language.name = name.string_value;
        language.grammar_path = grammar_path.string_value;
        if (!language.grammar_path.is_absolute()) {
            language.grammar_path = path.parent_path() / language.grammar_path;
        }
        language.symbol_name = symbol_name.string_value;
        language.highlights_path = highlights_path.string_value;
        if (!language.highlights_path.is_absolute()) {
            language.highlights_path = path.parent_path() / language.highlights_path;
        }
        for (const JsonValue &extension_value : extensions.array_value) {
            if (extension_value.type != JsonValue::Type::String) {
                throw std::runtime_error("syntax language extensions must be strings");
            }
            std::string extension = normalize_extension(extension_value.string_value);
            auto existing_owner = extension_owners.find(extension);
            if (existing_owner != extension_owners.end()) {
                throw std::runtime_error(
                    "duplicate syntax extension mapping for " + extension + ": " + existing_owner->second + " and " + language.name);
            }
            extension_owners.emplace(extension, language.name);
            language.extensions.push_back(std::move(extension));
        }

        if (language.name.empty() || language.symbol_name.empty() || language.extensions.empty()) {
            throw std::runtime_error("syntax language entries must not be empty");
        }
        parsed_languages.push_back(std::move(language));
    }

    return parsed_languages;
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
    config.syntax_config_path = first_existing_default_config_path("syntax.json");
    if (config.lsp_path && std::filesystem::exists(*config.lsp_path)) {
        config.lsp_servers = load_lsp_servers_from_path(*config.lsp_path);
    }
    if (config.syntax_config_path && std::filesystem::exists(*config.syntax_config_path)) {
        config.syntax_languages = load_syntax_languages_from_path(*config.syntax_config_path);
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
        } else if (key == "syntax_config") {
            config.syntax_config_path = resolve_config_reference(path, value);
        } else if (key == "log" || key == "log_file") {
            config.log_path = resolve_config_reference(path, value);
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
    if (!config.syntax_config_path) {
        std::filesystem::path default_syntax_path = resolve_config_reference(path, "syntax.json");
        if (std::filesystem::exists(default_syntax_path)) {
            config.syntax_config_path = default_syntax_path;
        }
    }
    if (config.lsp_path && std::filesystem::exists(*config.lsp_path)) {
        config.lsp_servers = load_lsp_servers_from_path(*config.lsp_path);
    } else if (config.lsp_command && config.lsp_language_id) {
        LspServerConfig fallback;
        fallback.name = *config.lsp_language_id;
        fallback.command = *config.lsp_command;
        fallback.language_id = *config.lsp_language_id;
        fallback.extensions.push_back("*");
        fallback.workspace.fallback = "current_working_directory";
        config.lsp_servers.push_back(std::move(fallback));
    }
    if (config.syntax_config_path) {
        if (!std::filesystem::exists(*config.syntax_config_path)) {
            throw std::runtime_error("configured syntax config file not found: " + config.syntax_config_path->string());
        }
        config.syntax_languages = load_syntax_languages_from_path(*config.syntax_config_path);
    }

    return config;
}

const LspServerConfig *matching_lsp_server(const EditorConfig &config, const std::optional<std::string> &file_path) {
    if (file_path && !file_path->empty()) {
        std::string extension = lowercase(std::filesystem::path(*file_path).extension().string());
        for (const LspServerConfig &server : config.lsp_servers) {
            for (const std::string &configured_extension : server.extensions) {
                if (configured_extension == extension) {
                    return &server;
                }
            }
        }
    }

    for (const LspServerConfig &server : config.lsp_servers) {
        for (const std::string &configured_extension : server.extensions) {
            if (configured_extension == "*") {
                return &server;
            }
        }
    }
    return nullptr;
}

std::string infer_language_id(const EditorConfig &config, const std::optional<std::string> &file_path) {
    if (const LspServerConfig *matched = matching_lsp_server(config, file_path)) {
        return matched->language_id;
    }

    if (file_path && !file_path->empty()) {
        std::string extension = lowercase(std::filesystem::path(*file_path).extension().string());
        if (extension == ".c" || extension == ".cc" || extension == ".cpp" || extension == ".cxx" ||
            extension == ".h" || extension == ".hh" || extension == ".hpp" || extension == ".hxx") {
            return "cpp";
        }
        if (extension == ".py" || extension == ".pyi" || extension == ".pyw") {
            return "python";
        }
        if (extension == ".json" || extension == ".jsonc") {
            return "json";
        }
    }

    if (config.lsp_language_id && !config.lsp_language_id->empty()) {
        return *config.lsp_language_id;
    }
    if (config.syntax_name && !config.syntax_name->empty()) {
        return *config.syntax_name;
    }
    return "text";
}

std::filesystem::path infer_workspace_root(const LspServerConfig &config, const std::optional<std::string> &file_path) {
    if (file_path && !file_path->empty()) {
        std::filesystem::path current = std::filesystem::path(*file_path).parent_path();
        if (!current.empty()) {
            while (true) {
                for (const std::string &marker : config.workspace.markers) {
                    if (std::filesystem::exists(current / marker)) {
                        return current;
                    }
                }
                std::filesystem::path parent = current.parent_path();
                if (parent.empty() || parent == current) {
                    break;
                }
                current = parent;
            }
            if (config.workspace.fallback == "file_directory") {
                return std::filesystem::path(*file_path).parent_path();
            }
        }
    }

    return std::filesystem::current_path();
}
