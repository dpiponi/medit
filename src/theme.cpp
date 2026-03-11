#include "theme.hpp"

#include "config.hpp"
#include "json.hpp"

#include <array>
#include <map>
#include <curses.h>
#include <stdexcept>

namespace {

constexpr const char *kEmbeddedDefaultTheme = R"json(
{
  "default_text": { "foreground": "default", "background": "default", "bold": "false", "underline": "false", "reverse": "false" },
  "line_number": { "foreground": "blue", "background": "default", "bold": "false", "underline": "false", "reverse": "false" },
  "cursor_line": { "foreground": "default", "background": "black", "bold": "false", "underline": "false", "reverse": "false" },
  "cursor_line_number": { "foreground": "cyan", "background": "black", "bold": "true", "underline": "false", "reverse": "false" },
  "status_bar": { "foreground": "black", "background": "cyan", "bold": "true", "underline": "false", "reverse": "false" },
  "message_bar": { "foreground": "yellow", "background": "default", "bold": "false", "underline": "false", "reverse": "false" },
  "command_line": { "foreground": "white", "background": "blue", "bold": "false", "underline": "false", "reverse": "false" },
  "selection": { "foreground": "black", "background": "white", "bold": "false", "underline": "false", "reverse": "false" },
  "search_match": { "foreground": "black", "background": "yellow", "bold": "true", "underline": "false", "reverse": "false" },
  "search_match_current": { "foreground": "black", "background": "green", "bold": "true", "underline": "false", "reverse": "false" },
  "syntax_keyword": { "foreground": "cyan", "background": "default", "bold": "true", "underline": "false", "reverse": "false" },
  "syntax_string": { "foreground": "green", "background": "default", "bold": "false", "underline": "false", "reverse": "false" },
  "syntax_comment": { "foreground": "blue", "background": "default", "bold": "false", "underline": "false", "reverse": "false" },
  "diagnostic_error": { "foreground": "red", "background": "default", "bold": "false", "underline": "true", "reverse": "false" },
  "diagnostic_warning": { "foreground": "yellow", "background": "default", "bold": "false", "underline": "true", "reverse": "false" }
}
)json";

std::size_t style_role_index(StyleRole role) {
    return static_cast<std::size_t>(role);
}

std::map<std::string, StyleRole> role_names() {
    return {
        {"default_text", StyleRole::DefaultText},
        {"line_number", StyleRole::LineNumber},
        {"cursor_line", StyleRole::CursorLine},
        {"cursor_line_number", StyleRole::CursorLineNumber},
        {"status_bar", StyleRole::StatusBar},
        {"message_bar", StyleRole::MessageBar},
        {"command_line", StyleRole::CommandLine},
        {"selection", StyleRole::Selection},
        {"search_match", StyleRole::SearchMatch},
        {"search_match_current", StyleRole::SearchMatchCurrent},
        {"syntax_keyword", StyleRole::SyntaxKeyword},
        {"syntax_string", StyleRole::SyntaxString},
        {"syntax_comment", StyleRole::SyntaxComment},
        {"diagnostic_error", StyleRole::DiagnosticError},
        {"diagnostic_warning", StyleRole::DiagnosticWarning},
    };
}

short parse_color_name(const std::string &name) {
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
    };
    auto found = colors.find(name);
    if (found == colors.end()) {
        throw std::runtime_error("unknown color: " + name);
    }
    return found->second;
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
    auto found = value.object_value.find(key);
    if (found == value.object_value.end()) {
        throw std::runtime_error(std::string("missing theme property: ") + key);
    }
    return found->second;
}

TextStyle parse_text_style(const JsonValue &value) {
    if (value.type != JsonValue::Type::Object) {
        throw std::runtime_error("theme role must be an object");
    }

    const JsonValue &foreground = required_member(value, "foreground");
    const JsonValue &background = required_member(value, "background");
    const JsonValue &bold = required_member(value, "bold");
    const JsonValue &underline = required_member(value, "underline");
    const JsonValue &reverse = required_member(value, "reverse");
    if (foreground.type != JsonValue::Type::String || background.type != JsonValue::Type::String ||
        bold.type != JsonValue::Type::String || underline.type != JsonValue::Type::String ||
        reverse.type != JsonValue::Type::String) {
        throw std::runtime_error("theme properties must be strings");
    }

    return {
        parse_color_name(foreground.string_value),
        parse_color_name(background.string_value),
        parse_bool_string(bold.string_value),
        parse_bool_string(underline.string_value),
        parse_bool_string(reverse.string_value),
    };
}

Theme parse_theme_source(const std::string &source, const std::string &origin) {
    JsonValue root = parse_json(source);
    if (root.type != JsonValue::Type::Object) {
        throw std::runtime_error("theme root must be an object");
    }

    Theme theme;
    theme.source_path = origin;
    for (const auto &[name, role] : role_names()) {
        auto found = root.object_value.find(name);
        if (found == root.object_value.end()) {
            throw std::runtime_error("missing theme role: " + name);
        }
        theme.styles[style_role_index(role)] = parse_text_style(found->second);
    }
    return theme;
}

}  // namespace

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
    return parse_theme_source(read_text_file(path), path.string());
}

Theme load_embedded_theme() {
    return parse_theme_source(kEmbeddedDefaultTheme, "<embedded>");
}

TextStyle theme_style(const Theme &theme, StyleRole role) {
    return theme.styles[style_role_index(role)];
}
