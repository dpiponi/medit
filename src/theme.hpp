#pragma once

#include "config.hpp"
#include "editor_core.hpp"

#include <array>
#include <filesystem>
#include <string>

struct Theme {
    std::array<TextStyle, static_cast<std::size_t>(StyleRole::DiagnosticSelected) + 1> styles;
    std::string source_path;
};

Theme load_theme();
Theme load_theme(const EditorConfig &config);
Theme load_theme_from_path(const std::filesystem::path &path);
Theme load_embedded_theme();
TextStyle theme_style(const Theme &theme, StyleRole role);
