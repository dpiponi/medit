#pragma once

#include <filesystem>
#include <optional>
#include <string>

struct EditorConfig {
    std::string source_path;
    std::optional<std::filesystem::path> keybindings_path;
    std::optional<std::filesystem::path> colors_path;
    std::optional<std::string> lsp_command;
    std::optional<std::string> lsp_language_id;
    bool right_justify_diagnostics = false;
};

EditorConfig load_editor_config();
EditorConfig load_editor_config_from_path(const std::filesystem::path &path);
