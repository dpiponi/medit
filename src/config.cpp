#include "config.hpp"
#include "json.hpp"
#include "string_utils.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ranges>

namespace {

bool parse_bool_value(const std::string &value) {
    if (value == "true") {
        return true;
    }
    if (value == "false") {
        return false;
    }
    throw std::runtime_error("invalid boolean value: " + value);
}

std::size_t parse_positive_size_value(const std::string &value, const std::string &key) {
    std::size_t parsed = 0;
    auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size() || parsed == 0) {
        throw std::runtime_error("invalid " + key + " value: " + value);
    }
    return parsed;
}

std::size_t parse_non_negative_size_value(const std::string &value, const std::string &key) {
    std::size_t parsed = 0;
    auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size()) {
        throw std::runtime_error("invalid " + key + " value: " + value);
    }
    return parsed;
}

ClipboardMode parse_clipboard_mode(const std::string &value) {
    if (value == "auto") {
        return ClipboardMode::Auto;
    }
    if (value == "native") {
        return ClipboardMode::Native;
    }
    if (value == "shared-file") {
        return ClipboardMode::SharedFile;
    }
    if (value == "internal") {
        return ClipboardMode::Internal;
    }
    throw std::runtime_error("invalid clipboard mode: " + value);
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

const JsonValue &required_patterns_member(const JsonValue &object, const char *config_kind) {
    auto patterns = object.object_value.find("patterns");
    if (patterns != object.object_value.end()) {
        return patterns->second;
    }
    auto extensions = object.object_value.find("extensions");
    if (extensions != object.object_value.end()) {
        return extensions->second;
    }
    throw std::runtime_error(std::string("missing ") + config_kind + " config field: patterns");
}

std::string pattern_from_legacy_extension(const std::string &extension) {
    if (extension == "*") {
        return "*";
    }
    return "*" + normalize_extension(extension);
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

SyntaxLanguageConfig::EditorSettings parse_syntax_editor_settings(const JsonValue &value) {
    if (value.type != JsonValue::Type::Object) {
        throw std::runtime_error("syntax language editor settings must be an object");
    }

    SyntaxLanguageConfig::EditorSettings settings;
    for (const auto &[key, setting_value] : value.object_value) {
        if (key == "shiftwidth") {
            if (setting_value.type != JsonValue::Type::Number) {
                throw std::runtime_error("syntax language shiftwidth must be a number");
            }
            if (setting_value.number_value <= 0 ||
                setting_value.number_value != static_cast<double>(static_cast<std::size_t>(setting_value.number_value))) {
                throw std::runtime_error("syntax language shiftwidth must be a positive integer");
            }
            settings.shiftwidth = static_cast<std::size_t>(setting_value.number_value);
        } else if (key == "tabstop") {
            if (setting_value.type != JsonValue::Type::Number) {
                throw std::runtime_error("syntax language tabstop must be a number");
            }
            if (setting_value.number_value <= 0 ||
                setting_value.number_value != static_cast<double>(static_cast<std::size_t>(setting_value.number_value))) {
                throw std::runtime_error("syntax language tabstop must be a positive integer");
            }
            settings.tabstop = static_cast<std::size_t>(setting_value.number_value);
        } else if (key == "softtabstop") {
            if (setting_value.type != JsonValue::Type::Number) {
                throw std::runtime_error("syntax language softtabstop must be a number");
            }
            if (setting_value.number_value < 0 ||
                setting_value.number_value != static_cast<double>(static_cast<std::size_t>(setting_value.number_value))) {
                throw std::runtime_error("syntax language softtabstop must be a non-negative integer");
            }
            settings.softtabstop = static_cast<std::size_t>(setting_value.number_value);
        } else if (key == "expandtab") {
            if (setting_value.type != JsonValue::Type::Bool) {
                throw std::runtime_error("syntax language expandtab must be a boolean");
            }
            settings.expandtab = setting_value.bool_value;
        } else if (key == "autoindent") {
            if (setting_value.type != JsonValue::Type::Bool) {
                throw std::runtime_error("syntax language autoindent must be a boolean");
            }
            settings.autoindent = setting_value.bool_value;
        } else if (key == "show_diagnostics_in_insert_mode") {
            if (setting_value.type != JsonValue::Type::Bool) {
                throw std::runtime_error("syntax language show_diagnostics_in_insert_mode must be a boolean");
            }
            settings.show_diagnostics_in_insert_mode = setting_value.bool_value;
        } else {
            throw std::runtime_error("unknown syntax language editor setting: " + key);
        }
    }
    return settings;
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
    std::map<std::string, std::string> pattern_owners;
    for (const JsonValue &server_value : servers->second.array_value) {
        if (server_value.type != JsonValue::Type::Object) {
            throw std::runtime_error("lsp server entries must be objects");
        }

        const JsonValue &name = required_object_member(server_value, "name");
        const JsonValue &command = required_object_member(server_value, "command");
        const JsonValue &language_id = required_object_member(server_value, "language_id");
        const JsonValue &patterns = required_patterns_member(server_value, "lsp server");
        if (name.type != JsonValue::Type::String || command.type != JsonValue::Type::String ||
            language_id.type != JsonValue::Type::String || patterns.type != JsonValue::Type::Array) {
            throw std::runtime_error("invalid lsp server field types");
        }

        LspServerConfig server;
        server.name = name.string_value;
        server.command = command.string_value;
        server.language_id = language_id.string_value;
        bool using_legacy_extensions = server_value.object_value.find("patterns") == server_value.object_value.end();
        for (const JsonValue &pattern_value : patterns.array_value) {
            if (pattern_value.type != JsonValue::Type::String) {
                throw std::runtime_error("lsp server patterns must be strings");
            }
            std::string pattern = using_legacy_extensions
                ? pattern_from_legacy_extension(pattern_value.string_value)
                : pattern_value.string_value;
            auto existing_owner = pattern_owners.find(pattern);
            if (existing_owner != pattern_owners.end()) {
                throw std::runtime_error(
                    "duplicate lsp pattern mapping for " + pattern + ": " + existing_owner->second + " and " + name.string_value);
            }
            pattern_owners.emplace(pattern, name.string_value);
            server.patterns.push_back(std::move(pattern));
        }
        if (server.name.empty() || server.command.empty() || server.language_id.empty() || server.patterns.empty()) {
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
    std::map<std::string, std::string> pattern_owners;
    for (const JsonValue &language_value : languages->second.array_value) {
        if (language_value.type != JsonValue::Type::Object) {
            throw std::runtime_error("syntax language entries must be objects");
        }

        const JsonValue &name = required_object_member(language_value, "name");
        const JsonValue &patterns = required_patterns_member(language_value, "syntax language");
        const JsonValue &grammar_path = required_object_member(language_value, "grammar_path");
        const JsonValue &symbol_name = required_object_member(language_value, "symbol_name");
        const JsonValue &highlights_path = required_object_member(language_value, "highlights_path");
        if (name.type != JsonValue::Type::String || patterns.type != JsonValue::Type::Array ||
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
        auto editor = language_value.object_value.find("editor");
        if (editor != language_value.object_value.end()) {
            language.editor = parse_syntax_editor_settings(editor->second);
        }
        bool using_legacy_extensions = language_value.object_value.find("patterns") == language_value.object_value.end();
        for (const JsonValue &pattern_value : patterns.array_value) {
            if (pattern_value.type != JsonValue::Type::String) {
                throw std::runtime_error("syntax language patterns must be strings");
            }
            std::string pattern = using_legacy_extensions
                ? pattern_from_legacy_extension(pattern_value.string_value)
                : pattern_value.string_value;
            auto existing_owner = pattern_owners.find(pattern);
            if (existing_owner != pattern_owners.end()) {
                throw std::runtime_error(
                    "duplicate syntax pattern mapping for " + pattern + ": " + existing_owner->second + " and " + language.name);
            }
            pattern_owners.emplace(pattern, language.name);
            language.patterns.push_back(std::move(pattern));
        }

        if (language.name.empty() || language.symbol_name.empty() || language.patterns.empty()) {
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
    config.clipboard = default_clipboard_config();
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
    config.clipboard = default_clipboard_config();
    config.source_path = path.string();

    std::string line;
    while (std::getline(input, line)) {
        std::string content = trim_ascii_whitespace(line);
        if (content.empty() || content[0] == '#') {
            continue;
        }
        std::size_t separator = content.find('=');
        if (separator == std::string::npos) {
            throw std::runtime_error("invalid config line: " + content);
        }

        std::string key = trim_ascii_whitespace(content.substr(0, separator));
        std::string value = trim_ascii_whitespace(content.substr(separator + 1));
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
        } else if (key == "show_diagnostics_in_insert_mode") {
            config.show_diagnostics_in_insert_mode = parse_bool_value(value);
        } else if (key == "clipboard") {
            config.clipboard.mode = parse_clipboard_mode(value);
        } else if (key == "clipboard_file") {
            config.clipboard.shared_file_path = value;
            if (!config.clipboard.shared_file_path.is_absolute()) {
                config.clipboard.shared_file_path = resolve_config_reference(path, value);
            }
        } else if (key == "clipboard_osc52") {
            config.clipboard.osc52 = parse_bool_value(value);
        } else if (key == "shiftwidth") {
            config.shiftwidth = parse_positive_size_value(value, key);
        } else if (key == "tabstop") {
            config.tabstop = parse_positive_size_value(value, key);
        } else if (key == "softtabstop") {
            config.softtabstop = parse_non_negative_size_value(value, key);
        } else if (key == "expandtab") {
            config.expandtab = parse_bool_value(value);
        } else if (key == "autoindent") {
            config.autoindent = parse_bool_value(value);
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
        fallback.patterns.push_back("*");
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
        for (const LspServerConfig &server : config.lsp_servers) {
            for (const std::string &pattern : server.patterns) {
                if (file_path_matches_glob(*file_path, pattern)) {
                    return &server;
                }
            }
        }
    }

    for (const LspServerConfig &server : config.lsp_servers) {
        for (const std::string &pattern : server.patterns) {
            if (pattern == "*") {
                return &server;
            }
        }
    }
    return nullptr;
}

const SyntaxLanguageConfig *matching_syntax_language(const EditorConfig &config, const std::optional<std::string> &file_path) {
    if (config.syntax_name && !config.syntax_name->empty()) {
        for (const SyntaxLanguageConfig &language : config.syntax_languages) {
            if (language.name == *config.syntax_name) {
                return &language;
            }
        }
    }

    if (file_path && !file_path->empty()) {
        for (const SyntaxLanguageConfig &language : config.syntax_languages) {
            for (const std::string &pattern : language.patterns) {
                if (file_path_matches_glob(*file_path, pattern)) {
                    return &language;
                }
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
        std::string extension = ascii_lowercase(std::filesystem::path(*file_path).extension().string());
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
                if (std::ranges::find_if(config.workspace.markers, [&current](const std::string &marker) {
                        return std::filesystem::exists(current / marker);
                    }) != config.workspace.markers.end()) {
                    return current;
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

std::size_t effective_shiftwidth(const EditorConfig &config, const std::optional<std::string> &file_path) {
    if (const SyntaxLanguageConfig *language = matching_syntax_language(config, file_path)) {
        if (language->editor.shiftwidth) {
            return *language->editor.shiftwidth;
        }
    }
    return config.shiftwidth;
}

std::size_t effective_tabstop(const EditorConfig &config, const std::optional<std::string> &file_path) {
    if (const SyntaxLanguageConfig *language = matching_syntax_language(config, file_path)) {
        if (language->editor.tabstop) {
            return *language->editor.tabstop;
        }
    }
    return config.tabstop;
}

std::size_t effective_softtabstop(const EditorConfig &config, const std::optional<std::string> &file_path) {
    if (const SyntaxLanguageConfig *language = matching_syntax_language(config, file_path)) {
        if (language->editor.softtabstop) {
            return *language->editor.softtabstop == 0 ? effective_shiftwidth(config, file_path) : *language->editor.softtabstop;
        }
    }
    return config.softtabstop == 0 ? effective_shiftwidth(config, file_path) : config.softtabstop;
}

bool effective_expandtab(const EditorConfig &config, const std::optional<std::string> &file_path) {
    if (const SyntaxLanguageConfig *language = matching_syntax_language(config, file_path)) {
        if (language->editor.expandtab) {
            return *language->editor.expandtab;
        }
    }
    return config.expandtab;
}

bool effective_autoindent(const EditorConfig &config, const std::optional<std::string> &file_path) {
    if (const SyntaxLanguageConfig *language = matching_syntax_language(config, file_path)) {
        if (language->editor.autoindent) {
            return *language->editor.autoindent;
        }
    }
    return config.autoindent;
}

bool effective_show_diagnostics_in_insert_mode(const EditorConfig &config, const std::optional<std::string> &file_path) {
    if (const SyntaxLanguageConfig *language = matching_syntax_language(config, file_path)) {
        if (language->editor.show_diagnostics_in_insert_mode) {
            return *language->editor.show_diagnostics_in_insert_mode;
        }
    }
    return config.show_diagnostics_in_insert_mode;
}
