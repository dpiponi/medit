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
