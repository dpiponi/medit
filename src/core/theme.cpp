module;

#include "config.hpp"
#include "json.hpp"

#include <charconv>
#include <array>
#include <map>
#include <optional>
#ifdef _WIN32
// Enable ncurses-compatible mode in PDCurses
#ifndef NCURSES_MOUSE_VERSION
#define NCURSES_MOUSE_VERSION 1
#endif
#endif
#include <curses.h>
#ifdef _WIN32
#include "pdcurses_compat.hpp"
#endif
#include <stdexcept>

module theme;

namespace {

constexpr const char *kEmbeddedDefaultTheme = R"json(
{
  "default_text": { "foreground": "default", "background": "default", "bold": "false", "underline": "false", "reverse": "false" },
  "line_number": { "foreground": "blue", "background": "default", "bold": "false", "underline": "false", "reverse": "false" },
  "window_divider": { "foreground": "blue", "background": "default", "bold": "false", "underline": "false", "reverse": "false" },
  "cursor_line": { "foreground": "default", "background": "black", "bold": "false", "underline": "false", "reverse": "false" },
  "cursor_line_number": { "foreground": "cyan", "background": "black", "bold": "true", "underline": "false", "reverse": "false" },
  "status_bar": { "foreground": "black", "background": "cyan", "bold": "true", "underline": "false", "reverse": "false" },
  "message_bar": { "foreground": "yellow", "background": "default", "bold": "false", "underline": "false", "reverse": "false" },
  "command_line": { "foreground": "white", "background": "blue", "bold": "false", "underline": "false", "reverse": "false" },
  "selection": { "foreground": "black", "background": "white", "bold": "false", "underline": "false", "reverse": "false" },
  "search_match": { "foreground": "black", "background": "yellow", "bold": "true", "underline": "false", "reverse": "false" },
  "search_match_current": { "foreground": "black", "background": "green", "bold": "true", "underline": "false", "reverse": "false" },
  "syntax_keyword": { "foreground": "cyan", "background": "default", "bold": "true", "underline": "false", "reverse": "false" },
  "syntax_type": { "foreground": "yellow", "background": "default", "bold": "true", "underline": "false", "reverse": "false" },
  "syntax_function": { "foreground": "white", "background": "default", "bold": "false", "underline": "false", "reverse": "false" },
  "syntax_builtin": { "foreground": "magenta", "background": "default", "bold": "false", "underline": "false", "reverse": "false" },
  "syntax_property": { "foreground": "cyan", "background": "default", "bold": "false", "underline": "false", "reverse": "false" },
  "syntax_constant": { "foreground": "yellow", "background": "default", "bold": "false", "underline": "false", "reverse": "false" },
  "syntax_number": { "foreground": "red", "background": "default", "bold": "false", "underline": "false", "reverse": "false" },
  "syntax_operator": { "foreground": "white", "background": "default", "bold": "true", "underline": "false", "reverse": "false" },
  "syntax_string": { "foreground": "green", "background": "default", "bold": "false", "underline": "false", "reverse": "false" },
  "syntax_comment": { "foreground": "blue", "background": "default", "bold": "false", "underline": "false", "reverse": "false" },
  "diagnostic_error": { "foreground": "red", "background": "default", "bold": "false", "underline": "true", "reverse": "false" },
  "diagnostic_warning": { "foreground": "yellow", "background": "default", "bold": "false", "underline": "true", "reverse": "false" },
  "diagnostic_message_error": { "foreground": "red", "background": "default", "bold": "false", "underline": "false", "reverse": "false" },
  "diagnostic_message_warning": { "foreground": "yellow", "background": "default", "bold": "false", "underline": "false", "reverse": "false" },
  "diagnostic_selected": { "foreground": "black", "background": "yellow", "bold": "false", "underline": "false", "reverse": "false" }
}
)json";

std::size_t style_role_index(StyleRole role) {
    return static_cast<std::size_t>(role);
}

std::map<std::string, StyleRole> role_names() {
    return {
        {"default_text", StyleRole::DefaultText},
        {"line_number", StyleRole::LineNumber},
        {"window_divider", StyleRole::WindowDivider},
        {"cursor_line", StyleRole::CursorLine},
        {"cursor_line_number", StyleRole::CursorLineNumber},
        {"status_bar", StyleRole::StatusBar},
        {"message_bar", StyleRole::MessageBar},
        {"command_line", StyleRole::CommandLine},
        {"selection", StyleRole::Selection},
        {"search_match", StyleRole::SearchMatch},
        {"search_match_current", StyleRole::SearchMatchCurrent},
        {"syntax_keyword", StyleRole::SyntaxKeyword},
        {"syntax_type", StyleRole::SyntaxType},
        {"syntax_function", StyleRole::SyntaxFunction},
        {"syntax_builtin", StyleRole::SyntaxBuiltin},
        {"syntax_property", StyleRole::SyntaxProperty},
        {"syntax_constant", StyleRole::SyntaxConstant},
        {"syntax_number", StyleRole::SyntaxNumber},
        {"syntax_operator", StyleRole::SyntaxOperator},
        {"syntax_string", StyleRole::SyntaxString},
        {"syntax_comment", StyleRole::SyntaxComment},
        {"diagnostic_error", StyleRole::DiagnosticError},
        {"diagnostic_warning", StyleRole::DiagnosticWarning},
        {"diagnostic_message_error", StyleRole::DiagnosticMessageError},
        {"diagnostic_message_warning", StyleRole::DiagnosticMessageWarning},
        {"diagnostic_selected", StyleRole::DiagnosticSelected},
    };
}

std::optional<short> try_parse_theme_color_impl(std::string_view name) {
    static const std::map<std::string, short> colors = {
        {"default", -1},
        {"black", COLOR_BLACK},
        {"red", COLOR_RED},
        {"green", COLOR_GREEN},
        {"yellow", COLOR_YELLOW},
        {"blue", COLOR_BLUE},
        {"magenta", COLOR_MAGENTA},
        {"cyan", COLOR_CYAN},
        {"white", COLOR_WHITE},
        {"bright_black", 8},
        {"bright_red", 9},
        {"bright_green", 10},
        {"bright_yellow", 11},
        {"bright_blue", 12},
        {"bright_magenta", 13},
        {"bright_cyan", 14},
        {"bright_white", 15},
    };
    auto found = colors.find(std::string(name));
    if (found != colors.end()) {
        return found->second;
    }

    auto parse_numeric = [](std::string_view value) -> std::optional<short> {
        unsigned parsed = 0;
        auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (error != std::errc{} || end != value.data() + value.size() || parsed > 255) {
            return std::nullopt;
        }
        return static_cast<short>(parsed);
    };

    if (std::optional<short> numeric = parse_numeric(name)) {
        return numeric;
    }
    if (name.starts_with("color")) {
        if (std::optional<short> numeric = parse_numeric(std::string_view(name).substr(5))) {
            return numeric;
        }
    }
    return std::nullopt;
}

short parse_color_name(const std::string &name) {
    if (std::optional<short> parsed = try_parse_theme_color_impl(name)) {
        return *parsed;
    }
    throw std::runtime_error("unknown color: " + name);
}

bool parse_bool_string(const std::string &value) {
    if (value == "true") {
        return true;
    }
    if (value == "false") {
        return false;
    }
    throw std::runtime_error("invalid boolean value: " + value);
}

const JsonValue &required_member(const JsonValue &value, const char *key) {
    auto found = value.find(key);
    if (found == value.end()) {
        throw std::runtime_error(std::string("missing theme property: ") + key);
    }
    return *found;
}

TextStyle parse_text_style(const JsonValue &value) {
    if (!value.is_object()) {
        throw std::runtime_error("theme role must be an object");
    }

    const JsonValue &foreground = required_member(value, "foreground");
    const JsonValue &background = required_member(value, "background");
    const JsonValue &bold = required_member(value, "bold");
    const JsonValue &underline = required_member(value, "underline");
    const JsonValue &reverse = required_member(value, "reverse");
    if (!foreground.is_string() || !background.is_string() || !bold.is_string() || !underline.is_string() ||
        !reverse.is_string()) {
        throw std::runtime_error("theme properties must be strings");
    }

    return {
        parse_color_name(foreground.get<std::string>()),
        parse_color_name(background.get<std::string>()),
        parse_bool_string(bold.get<std::string>()),
        parse_bool_string(underline.get<std::string>()),
        parse_bool_string(reverse.get<std::string>()),
    };
}

Theme parse_theme_source_with_defaults(const std::string &source, const std::string &origin, const Theme *defaults) {
    JsonValue root = parse_json(source);
    if (!root.is_object()) {
        throw std::runtime_error("theme root must be an object");
    }

    Theme theme;
    if (defaults != nullptr) {
        theme = *defaults;
    }
    theme.source_path = origin;
    const std::map<std::string, StyleRole> names = role_names();
    for (const auto &[name, value] : root.items()) {
        auto found = names.find(name);
        if (found == names.end()) {
            throw std::runtime_error("unknown theme role: " + name);
        }
        theme.styles[style_role_index(found->second)] = parse_text_style(value);
    }
    return theme;
}

}  // namespace

std::optional<short> try_parse_theme_color(std::string_view name) {
    return try_parse_theme_color_impl(name);
}

Theme load_theme() {
    return load_theme(load_editor_config());
}

Theme load_theme(const EditorConfig &config) {
    if (config.colors_path && std::filesystem::exists(*config.colors_path)) {
        return load_theme_from_path(*config.colors_path);
    }
    if (!config.source_path.empty() && config.colors_path) {
        throw std::runtime_error("configured colors file not found: " + config.colors_path->string());
    }
    return load_embedded_theme();
}

Theme load_theme_from_path(const std::filesystem::path &path) {
    Theme defaults = load_embedded_theme();
    return parse_theme_source_with_defaults(read_text_file(path), path.string(), &defaults);
}

Theme load_embedded_theme() {
    return parse_theme_source_with_defaults(kEmbeddedDefaultTheme, "<embedded>", nullptr);
}

TextStyle theme_style(const Theme &theme, StyleRole role) {
    return theme.styles[style_role_index(role)];
}
