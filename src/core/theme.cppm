module;

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

export module theme;

export import config;
import editor_core;

export struct Theme {
    std::array<TextStyle, static_cast<std::size_t>(StyleRole::DiagnosticSelected) + 1> styles;
    std::string source_path;
};

export Theme load_theme();
export Theme load_theme(const EditorConfig &config);
export Theme load_theme_from_path(const std::filesystem::path &path);
export Theme load_embedded_theme();
export TextStyle theme_style(const Theme &theme, StyleRole role);
export std::optional<short> try_parse_theme_color(std::string_view name);
