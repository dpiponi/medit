#pragma once

#include "clipboard.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

struct LspServerConfig {
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

struct SyntaxLanguageConfig {
    std::string name;
    std::vector<std::string> patterns;
    std::filesystem::path grammar_path;
    std::string symbol_name;
    std::filesystem::path highlights_path;
};

struct EditorConfig {
    std::string source_path;
    std::optional<std::filesystem::path> keybindings_path;
    std::optional<std::filesystem::path> colors_path;
    std::optional<std::filesystem::path> lsp_path;
    std::optional<std::filesystem::path> syntax_config_path;
    std::optional<std::filesystem::path> log_path;
    ClipboardConfig clipboard;
    std::optional<std::string> lsp_command;
    std::optional<std::string> lsp_language_id;
    std::vector<LspServerConfig> lsp_servers;
    std::vector<SyntaxLanguageConfig> syntax_languages;
    std::optional<std::string> syntax_name;
    bool right_justify_diagnostics = false;
};

EditorConfig load_editor_config();
EditorConfig load_editor_config_from_path(const std::filesystem::path &path);
const LspServerConfig *matching_lsp_server(const EditorConfig &config, const std::optional<std::string> &file_path);
std::string infer_language_id(const EditorConfig &config, const std::optional<std::string> &file_path);
std::filesystem::path infer_workspace_root(const LspServerConfig &config, const std::optional<std::string> &file_path);
