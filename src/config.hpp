#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

struct LspServerConfig {
    std::string name;
    std::string command;
    std::string language_id;
    std::vector<std::string> extensions;
};

struct EditorConfig {
    std::string source_path;
    std::optional<std::filesystem::path> keybindings_path;
    std::optional<std::filesystem::path> colors_path;
    std::optional<std::filesystem::path> lsp_path;
    std::optional<std::string> lsp_command;
    std::optional<std::string> lsp_language_id;
    std::vector<LspServerConfig> lsp_servers;
    std::optional<std::string> syntax_name;
    bool right_justify_diagnostics = false;
};

EditorConfig load_editor_config();
EditorConfig load_editor_config_from_path(const std::filesystem::path &path);
std::string infer_language_id(const EditorConfig &config, const std::optional<std::string> &file_path);
