module;

#include "theme.hpp"

export module theme;

export Theme load_theme();
export Theme load_theme(const EditorConfig &config);
export Theme load_theme_from_path(const std::filesystem::path &path);
export Theme load_embedded_theme();
export TextStyle theme_style(const Theme &theme, StyleRole role);
