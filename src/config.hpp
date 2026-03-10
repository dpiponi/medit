#pragma once

#include <filesystem>
#include <optional>
#include <string>

struct EditorConfig {
    std::string source_path;
    std::optional<std::filesystem::path> keybindings_path;
    std::optional<std::filesystem::path> colors_path;
};

EditorConfig load_editor_config();
EditorConfig load_editor_config_from_path(const std::filesystem::path &path);
