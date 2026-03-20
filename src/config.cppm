module;

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

export module config;

export import clipboard;

export struct LspServerConfig {
    struct WorkspaceConfig {
        std::vector<std::string> markers;
        std::string fallback = "file_directory";
    };

    std::string name;
    std::string command;
    std::string language_id;
    std::vector<std::string> patterns;
    WorkspaceConfig workspace;
};

export struct SyntaxLanguageConfig {
    struct EditorSettings {
        std::optional<std::size_t> shiftwidth;
        std::optional<std::size_t> tabstop;
        std::optional<std::size_t> softtabstop;
        std::optional<bool> expandtab;
        std::optional<bool> autoindent;
        std::optional<bool> show_diagnostics_in_insert_mode;
    };

    std::string name;
    std::vector<std::string> patterns;
    std::filesystem::path grammar_path;
    std::string symbol_name;
    std::filesystem::path highlights_path;
    EditorSettings editor;
};

export struct EditorConfig {
    std::string source_path;
    std::optional<std::filesystem::path> keybindings_path;
    std::optional<std::filesystem::path> colors_path;
    std::optional<std::filesystem::path> lsp_path;
    std::optional<std::filesystem::path> syntax_config_path;
    std::optional<std::filesystem::path> lua_path;
    std::optional<std::filesystem::path> log_path;
    std::optional<std::filesystem::path> control_socket_path;
    std::optional<std::string> ai_command;
    std::optional<std::string> ai_provider;
    std::optional<std::string> ai_model;
    ClipboardConfig clipboard;
    std::optional<std::string> lsp_command;
    std::optional<std::string> lsp_language_id;
    std::vector<LspServerConfig> lsp_servers;
    std::vector<SyntaxLanguageConfig> syntax_languages;
    std::optional<std::string> syntax_name;
    bool right_justify_diagnostics = false;
    bool show_diagnostics_in_insert_mode = true;
    std::size_t shiftwidth = 4;
    std::size_t tabstop = 8;
    std::size_t softtabstop = 0;
    bool expandtab = false;
    bool autoindent = false;
};

export EditorConfig load_editor_config();
export EditorConfig load_editor_config_from_path(const std::filesystem::path &path);
export const LspServerConfig *matching_lsp_server(const EditorConfig &config, const std::optional<std::string> &file_path);
export const SyntaxLanguageConfig *matching_syntax_language(
    const EditorConfig &config,
    const std::optional<std::string> &file_path);
export std::string infer_language_id(const EditorConfig &config, const std::optional<std::string> &file_path);
export std::filesystem::path infer_workspace_root(
    const LspServerConfig &config,
    const std::optional<std::string> &file_path);
export std::size_t effective_shiftwidth(const EditorConfig &config, const std::optional<std::string> &file_path);
export std::size_t effective_tabstop(const EditorConfig &config, const std::optional<std::string> &file_path);
export std::size_t effective_softtabstop(const EditorConfig &config, const std::optional<std::string> &file_path);
export bool effective_expandtab(const EditorConfig &config, const std::optional<std::string> &file_path);
export bool effective_autoindent(const EditorConfig &config, const std::optional<std::string> &file_path);
export bool effective_show_diagnostics_in_insert_mode(
    const EditorConfig &config,
    const std::optional<std::string> &file_path);
