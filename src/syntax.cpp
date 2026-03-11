#include "syntax.hpp"

#include <cctype>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace {

bool is_ascii_identifier_start(char32_t codepoint) {
    return (codepoint >= U'a' && codepoint <= U'z') || (codepoint >= U'A' && codepoint <= U'Z') || codepoint == U'_';
}

bool is_ascii_identifier_continue(char32_t codepoint) {
    return is_ascii_identifier_start(codepoint) || (codepoint >= U'0' && codepoint <= U'9');
}

bool is_space(char32_t codepoint) {
    return codepoint == U' ' || codepoint == U'\t';
}

bool is_cpp_keyword(const std::u32string &token) {
    static const std::unordered_set<std::string> keywords = {
        "alignas", "alignof", "asm", "auto", "bool", "break", "case", "catch", "char", "char8_t",
        "char16_t", "char32_t", "class", "concept", "const", "consteval", "constexpr", "constinit",
        "const_cast", "continue", "co_await", "co_return", "co_yield", "decltype", "default", "delete",
        "do", "double", "dynamic_cast", "else", "enum", "explicit", "export", "extern", "false", "float",
        "for", "friend", "goto", "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept",
        "nullptr", "operator", "private", "protected", "public", "register", "reinterpret_cast", "requires",
        "return", "short", "signed", "sizeof", "static", "static_assert", "static_cast", "struct", "switch",
        "template", "this", "thread_local", "throw", "true", "try", "typedef", "typeid", "typename",
        "union", "unsigned", "using", "virtual", "void", "volatile", "wchar_t", "while"};
    return keywords.find(u32_to_utf8(token)) != keywords.end();
}

void push_span(
    std::vector<std::vector<HighlightSpan>> &spans_by_line,
    std::size_t row,
    std::size_t start,
    std::size_t end,
    StyleRole role) {
    if (start >= end) {
        return;
    }
    spans_by_line[row].push_back({{{row, start}, {row, end}}, role, 5});
}

std::vector<std::vector<HighlightSpan>> highlight_cpp_document(const std::vector<std::u32string> &lines) {
    std::vector<std::vector<HighlightSpan>> spans_by_line(lines.size());
    bool in_block_comment = false;

    for (std::size_t row = 0; row < lines.size(); ++row) {
        const std::u32string &line = lines[row];
        std::size_t column = 0;
        bool leading_only = true;

        while (column < line.size()) {
            if (in_block_comment) {
                std::size_t start = column;
                while (column + 1 < line.size() && !(line[column] == U'*' && line[column + 1] == U'/')) {
                    ++column;
                }
                if (column + 1 < line.size()) {
                    column += 2;
                    in_block_comment = false;
                } else {
                    column = line.size();
                }
                push_span(spans_by_line, row, start, column, StyleRole::SyntaxComment);
                leading_only = false;
                continue;
            }

            char32_t codepoint = line[column];
            if (leading_only && is_space(codepoint)) {
                ++column;
                continue;
            }

            if (leading_only && codepoint == U'#') {
                std::size_t start = column;
                ++column;
                while (column < line.size() && is_space(line[column])) {
                    ++column;
                }
                while (column < line.size() && is_ascii_identifier_continue(line[column])) {
                    ++column;
                }
                push_span(spans_by_line, row, start, column, StyleRole::SyntaxKeyword);
                leading_only = false;
                continue;
            }

            leading_only = false;

            if (codepoint == U'/' && column + 1 < line.size() && line[column + 1] == U'/') {
                push_span(spans_by_line, row, column, line.size(), StyleRole::SyntaxComment);
                break;
            }

            if (codepoint == U'/' && column + 1 < line.size() && line[column + 1] == U'*') {
                std::size_t start = column;
                column += 2;
                while (column + 1 < line.size() && !(line[column] == U'*' && line[column + 1] == U'/')) {
                    ++column;
                }
                if (column + 1 < line.size()) {
                    column += 2;
                } else {
                    in_block_comment = true;
                    column = line.size();
                }
                push_span(spans_by_line, row, start, column, StyleRole::SyntaxComment);
                continue;
            }

            if (codepoint == U'"' || codepoint == U'\'') {
                char32_t quote = codepoint;
                std::size_t start = column;
                ++column;
                bool escaped = false;
                while (column < line.size()) {
                    char32_t current = line[column];
                    ++column;
                    if (escaped) {
                        escaped = false;
                        continue;
                    }
                    if (current == U'\\') {
                        escaped = true;
                        continue;
                    }
                    if (current == quote) {
                        break;
                    }
                }
                push_span(spans_by_line, row, start, column, StyleRole::SyntaxString);
                continue;
            }

            if (is_ascii_identifier_start(codepoint)) {
                std::size_t start = column;
                ++column;
                while (column < line.size() && is_ascii_identifier_continue(line[column])) {
                    ++column;
                }
                std::u32string token = line.substr(start, column - start);
                if (is_cpp_keyword(token)) {
                    push_span(spans_by_line, row, start, column, StyleRole::SyntaxKeyword);
                }
                continue;
            }

            ++column;
        }
    }

    return spans_by_line;
}

}  // namespace

SyntaxMode syntax_mode_from_name(const std::string &name) {
    if (name == "none") {
        return SyntaxMode::None;
    }
    if (name == "cpp") {
        return SyntaxMode::Cpp;
    }
    throw std::runtime_error("unknown syntax: " + name);
}

SyntaxMode detect_syntax_mode(const std::optional<std::string> &file_path) {
    if (!file_path || file_path->empty()) {
        return SyntaxMode::None;
    }

    std::string extension = std::filesystem::path(*file_path).extension().string();
    for (char &ch : extension) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }

    if (extension == ".c" || extension == ".cc" || extension == ".cpp" || extension == ".cxx" ||
        extension == ".h" || extension == ".hh" || extension == ".hpp" || extension == ".hxx") {
        return SyntaxMode::Cpp;
    }
    return SyntaxMode::None;
}

SyntaxMode resolve_syntax_mode(const EditorConfig &config, const std::optional<std::string> &file_path) {
    if (config.syntax_name && !config.syntax_name->empty()) {
        return syntax_mode_from_name(*config.syntax_name);
    }
    return detect_syntax_mode(file_path);
}

std::vector<std::vector<HighlightSpan>> highlight_document_syntax(
    const std::vector<std::u32string> &lines,
    SyntaxMode mode) {
    switch (mode) {
        case SyntaxMode::Cpp:
            return highlight_cpp_document(lines);
        case SyntaxMode::None:
            return std::vector<std::vector<HighlightSpan>>(lines.size());
    }
    return std::vector<std::vector<HighlightSpan>>(lines.size());
}
