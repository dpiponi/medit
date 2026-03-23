module;

#include "json.hpp"
#include "string_utils.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <optional>
#include <ranges>

module config;

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

std::string parse_ai_provider_value(const std::string &value) {
    std::string normalized = ascii_lowercase(value);
    if (normalized == "openai" || normalized == "mistral") {
        return normalized;
    }
    throw std::runtime_error("invalid ai_provider: " + value);
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

std::optional<std::filesystem::path> first_existing_config_path(const std::filesystem::path &relative_path) {
    std::filesystem::path local = std::filesystem::current_path() / relative_path;
    if (std::filesystem::exists(local)) {
        return local;
    }

    const char *home = std::getenv("HOME");
    if (home != nullptr) {
        std::filesystem::path global = std::filesystem::path(home) / relative_path;
        if (std::filesystem::exists(global)) {
            return global;
        }
    }

    return std::nullopt;
}

std::optional<std::filesystem::path> first_existing_meditrc_path() {
    return first_existing_config_path(std::filesystem::path(".config") / "meditrc");
}

std::optional<std::filesystem::path> first_existing_default_config_path(const std::string &file_name) {
    return first_existing_config_path(std::filesystem::path(".config") / "medit" / file_name);
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

void apply_default_lua_path(EditorConfig &config, const std::filesystem::path &meditrc_path) {
    if (config.lua_path) {
        return;
    }
    std::filesystem::path default_lua_path = resolve_config_reference(meditrc_path, "init.lua");
    if (std::filesystem::exists(default_lua_path)) {
        config.lua_path = std::move(default_lua_path);
    }
}

std::filesystem::path normalize_config_path(const std::filesystem::path &path) {
    std::error_code error;
    std::filesystem::path absolute = std::filesystem::absolute(path, error);
    if (error) {
        absolute = path;
    }
    std::filesystem::path normalized = std::filesystem::weakly_canonical(absolute, error);
    if (error) {
        normalized = absolute.lexically_normal();
    }
    return normalized;
}

std::string format_include_chain(const std::vector<std::filesystem::path> &chain) {
    std::ostringstream output;
    for (std::size_t index = 0; index < chain.size(); ++index) {
        if (index > 0) {
            output << " -> ";
        }
        output << chain[index].string();
    }
    return output.str();
}

std::vector<std::filesystem::path> include_chain_with(
    const std::vector<std::filesystem::path> &stack,
    const std::filesystem::path &path) {
    std::vector<std::filesystem::path> chain = stack;
    chain.push_back(path);
    return chain;
}

void ensure_file_exists_for_include(const std::filesystem::path &path, const char *config_kind) {
    if (std::filesystem::exists(path)) {
        return;
    }
    throw std::runtime_error(
        std::string(config_kind) + " included file not found: " + path.string());
}

void reject_include_cycle(
    const std::vector<std::filesystem::path> &stack,
    const std::filesystem::path &path,
    const char *config_kind) {
    if (std::ranges::find(stack, path) == stack.end()) {
        return;
    }
    throw std::runtime_error(
        std::string(config_kind) + " include cycle: " + format_include_chain(include_chain_with(stack, path)));
}

std::filesystem::path resolve_meditrc_include_path(
    const std::filesystem::path &including_path,
    const std::string &value) {
    std::filesystem::path path = value;
    if (path.is_absolute()) {
        return path;
    }
    return including_path.parent_path() / path;
}

const JsonValue &required_object_member(const JsonValue &object, const char *key) {
    auto found = object.find(key);
    if (found == object.end()) {
        throw std::runtime_error(std::string("missing lsp config field: ") + key);
    }
    return *found;
}

const JsonValue &required_patterns_member(const JsonValue &object, const char *config_kind) {
    auto patterns = object.find("patterns");
    if (patterns != object.end()) {
        return *patterns;
    }
    auto extensions = object.find("extensions");
    if (extensions != object.end()) {
        return *extensions;
    }
    throw std::runtime_error(std::string("missing ") + config_kind + " config field: patterns");
}

std::string pattern_from_legacy_extension(const std::string &extension);

std::vector<std::string> parse_patterns_member(const JsonValue &object, const char *config_kind) {
    const JsonValue &patterns = required_patterns_member(object, config_kind);
    if (!patterns.is_array()) {
        throw std::runtime_error(std::string(config_kind) + " patterns must be an array");
    }
    std::vector<std::string> parsed_patterns;
    bool using_legacy_extensions = !object.contains("patterns");
    for (const JsonValue &pattern_value : patterns) {
        if (!pattern_value.is_string()) {
            throw std::runtime_error(std::string(config_kind) + " patterns must be strings");
        }
        parsed_patterns.push_back(
            using_legacy_extensions
                ? pattern_from_legacy_extension(pattern_value.get<std::string>())
                : pattern_value.get<std::string>());
    }
    return parsed_patterns;
}

std::string pattern_from_legacy_extension(const std::string &extension) {
    if (extension == "*") {
        return "*";
    }
    return "*" + normalize_extension(extension);
}

LspServerConfig::WorkspaceConfig parse_workspace_config(const JsonValue &value) {
    if (!value.is_object()) {
        throw std::runtime_error("lsp workspace config must be an object");
    }

    LspServerConfig::WorkspaceConfig workspace;
    auto markers = value.find("markers");
    if (markers != value.end()) {
        if (!markers->is_array()) {
            throw std::runtime_error("lsp workspace markers must be an array");
        }
        for (const JsonValue &marker_value : *markers) {
            if (!marker_value.is_string()) {
                throw std::runtime_error("lsp workspace markers must be strings");
            }
            workspace.markers.push_back(marker_value.get<std::string>());
        }
    }

    auto fallback = value.find("fallback");
    if (fallback != value.end()) {
        if (!fallback->is_string()) {
            throw std::runtime_error("lsp workspace fallback must be a string");
        }
        workspace.fallback = fallback->get<std::string>();
    }

    if (workspace.fallback != "file_directory" && workspace.fallback != "current_working_directory") {
        throw std::runtime_error("unsupported lsp workspace fallback: " + workspace.fallback);
    }
    return workspace;
}

SyntaxLanguageConfig::EditorSettings parse_syntax_editor_settings(const JsonValue &value) {
    if (!value.is_object()) {
        throw std::runtime_error("syntax language editor settings must be an object");
    }

    SyntaxLanguageConfig::EditorSettings settings;
    for (const auto &[key, setting_value] : value.items()) {
        if (key == "shiftwidth") {
            if (!setting_value.is_number()) {
                throw std::runtime_error("syntax language shiftwidth must be a number");
            }
            double number_value = setting_value.get<double>();
            if (number_value <= 0 || number_value != static_cast<double>(static_cast<std::size_t>(number_value))) {
                throw std::runtime_error("syntax language shiftwidth must be a positive integer");
            }
            settings.shiftwidth = static_cast<std::size_t>(number_value);
        } else if (key == "tabstop") {
            if (!setting_value.is_number()) {
                throw std::runtime_error("syntax language tabstop must be a number");
            }
            double number_value = setting_value.get<double>();
            if (number_value <= 0 || number_value != static_cast<double>(static_cast<std::size_t>(number_value))) {
                throw std::runtime_error("syntax language tabstop must be a positive integer");
            }
            settings.tabstop = static_cast<std::size_t>(number_value);
        } else if (key == "softtabstop") {
            if (!setting_value.is_number()) {
                throw std::runtime_error("syntax language softtabstop must be a number");
            }
            double number_value = setting_value.get<double>();
            if (number_value < 0 || number_value != static_cast<double>(static_cast<std::size_t>(number_value))) {
                throw std::runtime_error("syntax language softtabstop must be a non-negative integer");
            }
            settings.softtabstop = static_cast<std::size_t>(number_value);
        } else if (key == "expandtab") {
            if (!setting_value.is_boolean()) {
                throw std::runtime_error("syntax language expandtab must be a boolean");
            }
            settings.expandtab = setting_value.get<bool>();
        } else if (key == "autoindent") {
            if (!setting_value.is_boolean()) {
                throw std::runtime_error("syntax language autoindent must be a boolean");
            }
            settings.autoindent = setting_value.get<bool>();
        } else if (key == "show_diagnostics_in_insert_mode") {
            if (!setting_value.is_boolean()) {
                throw std::runtime_error("syntax language show_diagnostics_in_insert_mode must be a boolean");
            }
            settings.show_diagnostics_in_insert_mode = setting_value.get<bool>();
        } else {
            throw std::runtime_error("unknown syntax language editor setting: " + key);
        }
    }
    return settings;
}

std::optional<std::vector<std::filesystem::path>> parse_json_include_paths(
    const JsonValue &root,
    const std::filesystem::path &path,
    const char *config_kind) {
    auto include = root.find("include");
    if (include == root.end()) {
        return std::nullopt;
    }
    if (!include->is_array()) {
        throw std::runtime_error(std::string(config_kind) + " include must be an array in " + path.string());
    }
    std::vector<std::filesystem::path> include_paths;
    for (const JsonValue &include_value : *include) {
        if (!include_value.is_string()) {
            throw std::runtime_error(std::string(config_kind) + " include entries must be strings in " + path.string());
        }
        std::filesystem::path include_path = include_value.get<std::string>();
        if (!include_path.is_absolute()) {
            include_path = path.parent_path() / include_path;
        }
        include_paths.push_back(include_path);
    }
    return include_paths;
}

LspServerConfig parse_lsp_server_override(const JsonValue &server_value, const std::filesystem::path &path) {
    if (!server_value.is_object()) {
        throw std::runtime_error("lsp server entries must be objects in " + path.string());
    }

    const JsonValue &name = required_object_member(server_value, "name");
    if (!name.is_string()) {
        throw std::runtime_error("invalid lsp server name type in " + path.string());
    }

    LspServerConfig server;
    server.name = name.get<std::string>();

    auto command = server_value.find("command");
    if (command != server_value.end()) {
        if (!command->is_string()) {
            throw std::runtime_error("invalid lsp server command type in " + path.string());
        }
        server.command = command->get<std::string>();
    }

    auto language_id = server_value.find("language_id");
    if (language_id != server_value.end()) {
        if (!language_id->is_string()) {
            throw std::runtime_error("invalid lsp server language_id type in " + path.string());
        }
        server.language_id = language_id->get<std::string>();
    }

    if (server_value.contains("patterns") || server_value.contains("extensions")) {
        server.patterns = parse_patterns_member(server_value, "lsp server");
    }

    auto workspace = server_value.find("workspace");
    if (workspace != server_value.end()) {
        server.workspace = parse_workspace_config(*workspace);
    }

    return server;
}

LspServerConfig merge_lsp_server(const LspServerConfig &base, const LspServerConfig &override) {
    LspServerConfig merged = base;
    if (!override.name.empty()) {
        merged.name = override.name;
    }
    if (!override.command.empty()) {
        merged.command = override.command;
    }
    if (!override.language_id.empty()) {
        merged.language_id = override.language_id;
    }
    if (!override.patterns.empty()) {
        merged.patterns = override.patterns;
    }
    if (!override.workspace.markers.empty()) {
        merged.workspace.markers = override.workspace.markers;
    }
    if (override.workspace.fallback != "file_directory") {
        merged.workspace.fallback = override.workspace.fallback;
    }
    return merged;
}

void validate_final_lsp_servers(const std::vector<LspServerConfig> &servers) {
    std::map<std::string, std::string> pattern_owners;
    for (const LspServerConfig &server : servers) {
        if (server.name.empty() || server.command.empty() || server.language_id.empty() || server.patterns.empty()) {
            throw std::runtime_error("merged lsp server is missing required fields: " + server.name);
        }
        for (const std::string &pattern : server.patterns) {
            auto existing_owner = pattern_owners.find(pattern);
            if (existing_owner != pattern_owners.end()) {
                throw std::runtime_error(
                    "duplicate lsp pattern mapping for " + pattern + ": " + existing_owner->second + " and " + server.name);
            }
            pattern_owners.emplace(pattern, server.name);
        }
    }
}

std::vector<LspServerConfig> parse_lsp_servers_array(const JsonValue &root, const std::filesystem::path &path) {
    auto servers = root.find("servers");
    if (servers == root.end()) {
        return {};
    }
    if (!servers->is_array()) {
        throw std::runtime_error("lsp config servers must be an array in " + path.string());
    }

    std::vector<LspServerConfig> parsed_servers;
    for (const JsonValue &server_value : *servers) {
        parsed_servers.push_back(parse_lsp_server_override(server_value, path));
    }
    return parsed_servers;
}

std::vector<LspServerConfig> merge_lsp_servers(
    const std::vector<LspServerConfig> &base,
    const std::vector<LspServerConfig> &override) {
    std::vector<LspServerConfig> merged = base;
    for (const LspServerConfig &server : override) {
        auto existing = std::ranges::find(merged, server.name, &LspServerConfig::name);
        if (existing == merged.end()) {
            merged.push_back(server);
        } else {
            *existing = merge_lsp_server(*existing, server);
        }
    }
    return merged;
}

std::vector<LspServerConfig> load_lsp_servers_from_path_recursive(
    const std::filesystem::path &path,
    std::vector<std::filesystem::path> &stack,
    std::vector<std::filesystem::path> &chain) {
    std::filesystem::path normalized_path = normalize_config_path(path);
    reject_include_cycle(stack, normalized_path, "lsp config");
    ensure_file_exists_for_include(normalized_path, "lsp config");

    std::string source = read_text_file(normalized_path);
    if (source.empty()) {
        throw std::runtime_error("could not open lsp config file: " + normalized_path.string());
    }

    JsonValue root = parse_json(source);
    if (!root.is_object()) {
        throw std::runtime_error("lsp config root must be an object");
    }

    auto include_paths = parse_json_include_paths(root, normalized_path, "lsp config");
    std::vector<LspServerConfig> merged_servers;
    stack.push_back(normalized_path);
    if (include_paths) {
        for (const std::filesystem::path &include_path : *include_paths) {
            merged_servers = merge_lsp_servers(
                merged_servers,
                load_lsp_servers_from_path_recursive(include_path, stack, chain));
        }
    }
    stack.pop_back();

    if (!root.contains("servers") && !include_paths) {
        throw std::runtime_error("lsp config must contain a servers array");
    }

    merged_servers = merge_lsp_servers(merged_servers, parse_lsp_servers_array(root, normalized_path));
    validate_final_lsp_servers(merged_servers);
    chain.push_back(normalized_path);
    return merged_servers;
}

std::vector<LspServerConfig> load_lsp_servers_from_path(
    const std::filesystem::path &path,
    std::vector<std::filesystem::path> *chain_out = nullptr) {
    std::vector<std::filesystem::path> stack;
    std::vector<std::filesystem::path> chain;
    std::vector<LspServerConfig> servers = load_lsp_servers_from_path_recursive(path, stack, chain);
    if (chain_out != nullptr) {
        *chain_out = chain;
    }
    return servers;
}

SyntaxLanguageConfig parse_syntax_language_override(const JsonValue &language_value, const std::filesystem::path &path) {
    if (!language_value.is_object()) {
        throw std::runtime_error("syntax language entries must be objects in " + path.string());
    }

    const JsonValue &name = required_object_member(language_value, "name");
    if (!name.is_string()) {
        throw std::runtime_error("invalid syntax language name type in " + path.string());
    }

    SyntaxLanguageConfig language;
    language.name = name.get<std::string>();

    if (language_value.contains("patterns") || language_value.contains("extensions")) {
        language.patterns = parse_patterns_member(language_value, "syntax language");
    }

    auto grammar_path = language_value.find("grammar_path");
    if (grammar_path != language_value.end()) {
        if (!grammar_path->is_string()) {
            throw std::runtime_error("invalid syntax grammar_path type in " + path.string());
        }
        language.grammar_path = grammar_path->get<std::string>();
        if (!language.grammar_path.is_absolute()) {
            language.grammar_path = path.parent_path() / language.grammar_path;
        }
    }

    auto symbol_name = language_value.find("symbol_name");
    if (symbol_name != language_value.end()) {
        if (!symbol_name->is_string()) {
            throw std::runtime_error("invalid syntax symbol_name type in " + path.string());
        }
        language.symbol_name = symbol_name->get<std::string>();
    }

    auto highlights_path = language_value.find("highlights_path");
    if (highlights_path != language_value.end()) {
        if (!highlights_path->is_string()) {
            throw std::runtime_error("invalid syntax highlights_path type in " + path.string());
        }
        language.highlights_path = highlights_path->get<std::string>();
        if (!language.highlights_path.is_absolute()) {
            language.highlights_path = path.parent_path() / language.highlights_path;
        }
    }

    auto editor = language_value.find("editor");
    if (editor != language_value.end()) {
        language.editor = parse_syntax_editor_settings(*editor);
    }

    return language;
}

SyntaxLanguageConfig::EditorSettings merge_syntax_editor_settings(
    const SyntaxLanguageConfig::EditorSettings &base,
    const SyntaxLanguageConfig::EditorSettings &override) {
    SyntaxLanguageConfig::EditorSettings merged = base;
    if (override.shiftwidth.has_value()) {
        merged.shiftwidth = override.shiftwidth;
    }
    if (override.tabstop.has_value()) {
        merged.tabstop = override.tabstop;
    }
    if (override.softtabstop.has_value()) {
        merged.softtabstop = override.softtabstop;
    }
    if (override.expandtab.has_value()) {
        merged.expandtab = override.expandtab;
    }
    if (override.autoindent.has_value()) {
        merged.autoindent = override.autoindent;
    }
    if (override.show_diagnostics_in_insert_mode.has_value()) {
        merged.show_diagnostics_in_insert_mode = override.show_diagnostics_in_insert_mode;
    }
    return merged;
}

SyntaxLanguageConfig merge_syntax_language(const SyntaxLanguageConfig &base, const SyntaxLanguageConfig &override) {
    SyntaxLanguageConfig merged = base;
    if (!override.name.empty()) {
        merged.name = override.name;
    }
    if (!override.patterns.empty()) {
        merged.patterns = override.patterns;
    }
    if (!override.grammar_path.empty()) {
        merged.grammar_path = override.grammar_path;
    }
    if (!override.symbol_name.empty()) {
        merged.symbol_name = override.symbol_name;
    }
    if (!override.highlights_path.empty()) {
        merged.highlights_path = override.highlights_path;
    }
    merged.editor = merge_syntax_editor_settings(merged.editor, override.editor);
    return merged;
}

void validate_final_syntax_languages(const std::vector<SyntaxLanguageConfig> &languages) {
    std::map<std::string, std::string> pattern_owners;
    for (const SyntaxLanguageConfig &language : languages) {
        if (language.name.empty() || language.grammar_path.empty() || language.symbol_name.empty() ||
            language.highlights_path.empty() || language.patterns.empty()) {
            throw std::runtime_error("merged syntax language is missing required fields: " + language.name);
        }
        for (const std::string &pattern : language.patterns) {
            auto existing_owner = pattern_owners.find(pattern);
            if (existing_owner != pattern_owners.end()) {
                throw std::runtime_error(
                    "duplicate syntax pattern mapping for " + pattern + ": " + existing_owner->second + " and " + language.name);
            }
            pattern_owners.emplace(pattern, language.name);
        }
    }
}

std::vector<SyntaxLanguageConfig> parse_syntax_languages_array(const JsonValue &root, const std::filesystem::path &path) {
    auto languages = root.find("languages");
    if (languages == root.end()) {
        return {};
    }
    if (!languages->is_array()) {
        throw std::runtime_error("syntax config languages must be an array in " + path.string());
    }

    std::vector<SyntaxLanguageConfig> parsed_languages;
    for (const JsonValue &language_value : *languages) {
        parsed_languages.push_back(parse_syntax_language_override(language_value, path));
    }
    return parsed_languages;
}

std::vector<SyntaxLanguageConfig> merge_syntax_languages(
    const std::vector<SyntaxLanguageConfig> &base,
    const std::vector<SyntaxLanguageConfig> &override) {
    std::vector<SyntaxLanguageConfig> merged = base;
    for (const SyntaxLanguageConfig &language : override) {
        auto existing = std::ranges::find(merged, language.name, &SyntaxLanguageConfig::name);
        if (existing == merged.end()) {
            merged.push_back(language);
        } else {
            *existing = merge_syntax_language(*existing, language);
        }
    }
    return merged;
}

std::vector<SyntaxLanguageConfig> load_syntax_languages_from_path_recursive(
    const std::filesystem::path &path,
    std::vector<std::filesystem::path> &stack,
    std::vector<std::filesystem::path> &chain) {
    std::filesystem::path normalized_path = normalize_config_path(path);
    reject_include_cycle(stack, normalized_path, "syntax config");
    ensure_file_exists_for_include(normalized_path, "syntax config");

    std::string source = read_text_file(normalized_path);
    if (source.empty()) {
        throw std::runtime_error("could not open syntax config file: " + normalized_path.string());
    }

    JsonValue root = parse_json(source);
    if (!root.is_object()) {
        throw std::runtime_error("syntax config root must be an object");
    }

    auto include_paths = parse_json_include_paths(root, normalized_path, "syntax config");
    std::vector<SyntaxLanguageConfig> merged_languages;
    stack.push_back(normalized_path);
    if (include_paths) {
        for (const std::filesystem::path &include_path : *include_paths) {
            merged_languages = merge_syntax_languages(
                merged_languages,
                load_syntax_languages_from_path_recursive(include_path, stack, chain));
        }
    }
    stack.pop_back();

    if (!root.contains("languages") && !include_paths) {
        throw std::runtime_error("syntax config must contain a languages array");
    }

    merged_languages = merge_syntax_languages(merged_languages, parse_syntax_languages_array(root, normalized_path));
    validate_final_syntax_languages(merged_languages);
    chain.push_back(normalized_path);
    return merged_languages;
}

std::vector<SyntaxLanguageConfig> load_syntax_languages_from_path(
    const std::filesystem::path &path,
    std::vector<std::filesystem::path> *chain_out = nullptr) {
    std::vector<std::filesystem::path> stack;
    std::vector<std::filesystem::path> chain;
    std::vector<SyntaxLanguageConfig> languages = load_syntax_languages_from_path_recursive(path, stack, chain);
    if (chain_out != nullptr) {
        *chain_out = chain;
    }
    return languages;
}

void apply_meditrc_setting(EditorConfig &config, const std::filesystem::path &path, const std::string &key, const std::string &value) {
    if (key == "keybindings") {
        config.keybindings_path = resolve_config_reference(path, value);
    } else if (key == "colors") {
        config.colors_path = resolve_config_reference(path, value);
    } else if (key == "lsp") {
        config.lsp_path = resolve_config_reference(path, value);
    } else if (key == "syntax_config") {
        config.syntax_config_path = resolve_config_reference(path, value);
    } else if (key == "lua") {
        config.lua_path = resolve_config_reference(path, value);
    } else if (key == "log" || key == "log_file") {
        config.log_path = resolve_config_reference(path, value);
    } else if (key == "control_socket") {
        config.control_socket_path = value;
        if (!config.control_socket_path->is_absolute()) {
            config.control_socket_path = resolve_config_reference(path, value);
        }
    } else if (key == "ai_command") {
        config.ai_command = value;
    } else if (key == "ai_provider") {
        config.ai_provider = parse_ai_provider_value(value);
    } else if (key == "ai_model") {
        config.ai_model = value;
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

void load_editor_config_from_path_recursive(
    const std::filesystem::path &path,
    std::vector<std::filesystem::path> &stack,
    EditorConfig &config) {
    std::filesystem::path normalized_path = normalize_config_path(path);
    reject_include_cycle(stack, normalized_path, "meditrc");

    std::ifstream input(normalized_path);
    if (!input) {
        throw std::runtime_error("could not open config file: " + normalized_path.string());
    }

    stack.push_back(normalized_path);
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

        if (key == "include") {
            std::filesystem::path include_path = normalize_config_path(resolve_meditrc_include_path(normalized_path, value));
            ensure_file_exists_for_include(include_path, "meditrc");
            load_editor_config_from_path_recursive(include_path, stack, config);
            continue;
        }

        apply_meditrc_setting(config, normalized_path, key, value);
    }
    stack.pop_back();
    config.meditrc_chain.push_back(normalized_path);
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
    config.lua_path = first_existing_default_config_path("init.lua");
    if (config.lsp_path && std::filesystem::exists(*config.lsp_path)) {
        config.lsp_servers = load_lsp_servers_from_path(*config.lsp_path, &config.lsp_chain);
    }
    if (config.syntax_config_path && std::filesystem::exists(*config.syntax_config_path)) {
        config.syntax_languages = load_syntax_languages_from_path(*config.syntax_config_path, &config.syntax_chain);
    }
    return config;
}

EditorConfig load_editor_config_from_path(const std::filesystem::path &path) {
    EditorConfig config;
    config.clipboard = default_clipboard_config();
    std::filesystem::path normalized_path = normalize_config_path(path);
    config.source_path = normalized_path.string();

    std::vector<std::filesystem::path> stack;
    load_editor_config_from_path_recursive(normalized_path, stack, config);

    if (!config.keybindings_path) {
        config.keybindings_path = resolve_config_reference(normalized_path, "keybindings.json");
    }
    if (!config.colors_path) {
        config.colors_path = resolve_config_reference(normalized_path, "colors.json");
    }
    if (!config.lsp_path) {
        config.lsp_path = resolve_config_reference(normalized_path, "lsp.json");
    }
    if (!config.syntax_config_path) {
        std::filesystem::path default_syntax_path = resolve_config_reference(normalized_path, "syntax.json");
        if (std::filesystem::exists(default_syntax_path)) {
            config.syntax_config_path = default_syntax_path;
        }
    }
    apply_default_lua_path(config, normalized_path);
    if (config.lsp_path && std::filesystem::exists(*config.lsp_path)) {
        config.lsp_servers = load_lsp_servers_from_path(*config.lsp_path, &config.lsp_chain);
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
        config.syntax_languages = load_syntax_languages_from_path(*config.syntax_config_path, &config.syntax_chain);
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
        if (extension == ".lua") {
            return "lua";
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
