#include "editor_internal.hpp"
#include "editor_ex_command_completion.hpp"

#include "logger.hpp"
#include "process_utils.hpp"
#include "string_utils.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>
#endif

void EditorState::handle_quit_command(bool force) {
    if (!force && !can_quit_without_force(*this)) {
        set_status("Unsaved changes; use :q! to quit");
        return;
    }
    quit_editor();
}

void EditorState::handle_write_command(const std::string &argument) {
    EditorCore &core = active_core();
    if (argument.empty()) {
        if (core.save_current_file()) {
            set_status(prefixed_message("Wrote ", core.display_file_name()));
        } else {
            set_status(core.file_path() ? "Write failed" : "No file name");
        }
        return;
    }
    if (core.save_current_file_as(argument)) {
        set_status(prefixed_message("Wrote ", argument));
    } else {
        set_status("Write failed");
    }
}

void EditorState::handle_edit_command(const std::string &argument) {
    if (argument.empty()) {
        set_status("No file name");
        return;
    }
    std::string path = expand_user_path(argument);
    log_debug("edit command open path=" + path);
    EditorBuffer *buffer = session.open_file(path, true);
    if (buffer) {
        windows.set_active_buffer_id(buffer->id);
        sync_active_window_buffer();
        window_ui(windows.active_window_id()) = WindowUiState{};
        log_debug("edit command opened path=" + path);
        set_status(prefixed_message("Opened ", path));
    } else {
        log_debug("edit command open failed path=" + path);
        set_status("Could not open file");
    }
}

void EditorState::handle_write_quit_command(const std::string &argument) {
    EditorCore &core = active_core();
    if (argument.empty()) {
        if (!core.save_current_file()) {
            set_status(core.file_path() ? "Write failed" : "No file name");
            return;
        }
        if (!can_quit_without_force(*this)) {
            set_status("Other buffers still have unsaved changes");
            return;
        }
        quit_editor();
        return;
    }
    if (!core.save_current_file_as(argument)) {
        set_status("Write failed");
        return;
    }
    if (!can_quit_without_force(*this)) {
        set_status("Other buffers still have unsaved changes");
        return;
    }
    quit_editor();
}

std::string buffers_summary(const EditorState &state) {
    std::ostringstream message;
    const std::vector<EditorBuffer> &buffers = state.session.buffers();
    for (std::size_t index = 0; index < buffers.size(); ++index) {
        if (index > 0) {
            message << " | ";
        }
        const EditorBuffer &buffer = buffers[index];
        if (buffer.id == state.active_window().buffer_id) {
            message << "*";
        }
        message << buffer.id << ":" << buffer.core.display_file_name();
        if (buffer.core.is_dirty()) {
            message << "[+]";
        }
    }
    return message.str();
}

void EditorState::handle_buffer_switch_command(const std::string &argument) {
    if (argument.empty()) {
        set_status("No buffer id");
        return;
    }
    std::size_t buffer_id = 0;
    try {
        buffer_id = static_cast<std::size_t>(std::stoul(argument));
    } catch (const std::exception &) {
        set_status("Invalid buffer id");
        return;
    }
    if (session.find_buffer_by_id(buffer_id)) {
        show_buffer_in_active_window(buffer_id);
        set_status(prefixed_message("Switched to ", active_core().display_file_name()));
    } else {
        set_status("No such buffer");
    }
}

void EditorState::handle_buffer_delete_command(bool force) {
    std::size_t closing_id = active_window().buffer_id;
    std::string closing_name = active_core().display_file_name();
    session.switch_to_id(closing_id);
    EditorEvents closed_events;
    if (!session.close_active_buffer(force, &closed_events)) {
        set_status("Unsaved changes; use :bd! to close");
        return;
    }
    for (const EditorEvent &event : closed_events) {
        dispatch_editor_event(event);
    }
    buffer_ui_map.erase(closing_id);
    syntax_ui_map.erase(closing_id);
    std::size_t replacement_buffer_id = session.active_buffer_id();
    windows.replace_buffer_id(closing_id, replacement_buffer_id);
    for (const EditorWindow &window : windows.windows()) {
        if (window.buffer_id == replacement_buffer_id) {
            window_ui(window.id) = EditorState::WindowUiState{};
        }
    }
    sync_active_window_buffer();
    active_buffer_ui();
    set_status(prefixed_message("Closed ", closing_name));
}

void EditorState::handle_goto_line_command(const std::string &argument) {
    if (argument.empty()) {
        set_status("No line number");
        return;
    }

    std::size_t line_number = 0;
    try {
        line_number = static_cast<std::size_t>(std::stoul(argument));
    } catch (const std::exception &) {
        set_status("Invalid line number");
        return;
    }

    if (line_number == 0) {
        set_status("Line numbers start at 1");
        return;
    }

    EditorCore &core = active_core();
    std::size_t target_row = std::min(line_number - 1, core.line_count() - 1);
    core.set_cursor({target_row, 0});
    set_status("Line " + std::to_string(target_row + 1));
}

std::string shell_single_quote(const std::string &text) {
    std::string quoted = "'";
    for (char ch : text) {
        if (ch == '\'') {
            quoted += "'\\''";
        } else {
            quoted.push_back(ch);
        }
    }
    quoted.push_back('\'');
    return quoted;
}

std::string project_file_list_command(const std::optional<std::filesystem::path> &root = std::nullopt) {
    if (std::optional<std::string> fd = first_available_executable({"fd", "fdfind"})) {
        if (root && !root->empty()) {
            return *fd + " -t f -H -I -E .git . " + shell_single_quote(root->string());
        }
        return *fd + " -t f -H -I -E .git";
    }
    if (root && !root->empty()) {
        return "rg --files --hidden --no-ignore -g '!.git' " + shell_single_quote(root->string());
    }
    return "rg --files --hidden --no-ignore -g '!.git'";
}

std::string shell_printf_lines_command(const std::vector<std::string> &lines) {
    std::string command = "printf '%s\\n'";
    for (const std::string &line : lines) {
        command += " ";
        command += shell_single_quote(line);
    }
    return command;
}

std::optional<std::string> EditorState::run_picker_command(const std::string &pipeline_command, std::string &error_message) {
#if defined(__unix__) || defined(__APPLE__)
    if (std::optional<std::string> missing = missing_executable_in_pipeline(pipeline_command)) {
        error_message = "missing executable: " + *missing;
        log_debug("picker preflight failed missing executable=" + *missing + " pipeline=" + pipeline_command);
        return std::nullopt;
    }
    char temp_path[] = "/tmp/medit-picker-XXXXXX";
    int fd = mkstemp(temp_path);
    if (fd < 0) {
        error_message = "could not create temporary file";
        log_debug("picker temp file creation failed");
        return std::nullopt;
    }
    close(fd);

    const std::string current_directory = std::filesystem::current_path().string();
    std::string shell_command =
        "sh -c " + shell_single_quote("cd " + shell_single_quote(current_directory) + " && " + pipeline_command) +
        " > " + shell_single_quote(temp_path);
    log_debug("picker start cwd=" + current_directory + " pipeline=" + pipeline_command + " output=" + temp_path);
    log_debug("external command kind=picker spawn command=" + shell_command);
    pending.tokens.clear();
    pending.motion = PendingMotion::None;
    pending.motion_repeat_count = 1;
    pending.repeat_digits.clear();
    def_prog_mode();
    endwin();
    restore_shell_terminal_state();
    int result = std::system(shell_command.c_str());
    reset_prog_mode();
    refresh();
    clearok(stdscr, TRUE);
    log_debug("picker exit code=" + std::to_string(result));
    log_debug("external command kind=picker exit command=" + shell_command + " status=" + std::to_string(result));

    std::ifstream input(temp_path);
    std::string selection;
    std::getline(input, selection);
    const std::string raw_selection = selection;
    if (!selection.empty() && selection.back() == '\r') {
        selection.pop_back();
    }
    log_debug("picker raw selection=[" + raw_selection + "] normalized=[" + selection + "]");
    std::filesystem::remove(temp_path);

    if (result != 0) {
        error_message = "picker canceled";
        return std::nullopt;
    }
    if (selection.empty()) {
        error_message = "no selection";
        return std::nullopt;
    }
    return selection;
#else
    (void)pipeline_command;
    log_debug("picker unsupported on this platform");
    error_message = "external pickers unsupported on this platform";
    return std::nullopt;
#endif
}

std::filesystem::path theme_directory_for_config(const EditorConfig &config) {
    if (!config.source_path.empty()) {
        return std::filesystem::path(config.source_path).parent_path() / "medit" / "themes";
    }
    if (config.colors_path) {
        std::filesystem::path parent = config.colors_path->parent_path();
        if (parent.filename() == "themes") {
            return parent;
        }
        return parent / "themes";
    }
    return {};
}

bool update_meditrc_setting(
    const std::filesystem::path &meditrc_path,
    const std::string &key,
    const std::string &value,
    std::string &error_message) {
    std::ifstream input(meditrc_path);
    if (!input) {
        error_message = "could not open meditrc";
        return false;
    }

    std::vector<std::string> lines;
    std::string line;
    bool replaced = false;
    while (std::getline(input, line)) {
        std::string trimmed = trim_ascii_whitespace(line);
        std::size_t separator = trimmed.find('=');
        if (!trimmed.empty() && trimmed[0] != '#' && separator != std::string::npos &&
            trim_ascii_whitespace(trimmed.substr(0, separator)) == key) {
            lines.push_back(key + " = " + value);
            replaced = true;
        } else {
            lines.push_back(line);
        }
    }
    if (!replaced) {
        lines.push_back(key + " = " + value);
    }

    std::ofstream output(meditrc_path, std::ios::trunc);
    if (!output) {
        error_message = "could not write meditrc";
        return false;
    }
    for (std::size_t index = 0; index < lines.size(); ++index) {
        output << lines[index];
        if (index + 1 < lines.size()) {
            output << '\n';
        }
    }
    return true;
}

bool parse_substitute_command(std::string_view command, SubstituteCommand &parsed, std::string &error_message);

void EditorState::handle_pick_theme_command() {
    if (config.source_path.empty()) {
        set_status("Theme picker needs a meditrc");
        return;
    }

    std::filesystem::path theme_dir = theme_directory_for_config(config);
    if (theme_dir.empty() || !std::filesystem::exists(theme_dir)) {
        set_status("No theme directory");
        return;
    }

    std::vector<std::string> theme_names;
    for (const auto &entry : std::filesystem::directory_iterator(theme_dir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") {
            continue;
        }
        theme_names.push_back("themes/" + entry.path().filename().string());
    }
    std::sort(theme_names.begin(), theme_names.end());
    if (theme_names.empty()) {
        set_status("No themes found");
        return;
    }

    std::string error_message;
    std::optional<std::string> selection =
        run_picker_command(shell_printf_lines_command(theme_names) + " | fzf", error_message);
    if (!selection) {
        set_status(error_message);
        return;
    }

    std::string update_error;
    std::filesystem::path meditrc_path = config.source_path;
    if (!update_meditrc_setting(meditrc_path, "colors", *selection, update_error)) {
        set_status(update_error);
        return;
    }

    std::string reload_error;
    if (reload_editor_configuration(reload_error)) {
        set_status("Theme: " + *selection);
    } else {
        set_status("Config reload failed: " + reload_error);
    }
}

bool EditorState::run_filter_command(
    const std::string &command,
    const std::u32string &input_text,
    std::u32string &output_text,
    std::string &error_message) {
#if defined(__unix__) || defined(__APPLE__)
    char input_path[] = "/tmp/medit-filter-in-XXXXXX";
    int input_fd = mkstemp(input_path);
    if (input_fd < 0) {
        error_message = "could not create filter input file";
        return false;
    }
    close(input_fd);

    char output_path[] = "/tmp/medit-filter-out-XXXXXX";
    int output_fd = mkstemp(output_path);
    if (output_fd < 0) {
        std::filesystem::remove(input_path);
        error_message = "could not create filter output file";
        return false;
    }
    close(output_fd);

    char error_path[] = "/tmp/medit-filter-err-XXXXXX";
    int stderr_fd = mkstemp(error_path);
    if (stderr_fd < 0) {
        std::filesystem::remove(input_path);
        std::filesystem::remove(output_path);
        error_message = "could not create filter error file";
        return false;
    }
    close(stderr_fd);

    {
        std::ofstream input_file(input_path, std::ios::binary);
        input_file << u32_to_utf8(input_text);
    }

    std::string shell_command =
        "sh -c " + shell_single_quote(command) +
        " < " + shell_single_quote(input_path) +
        " > " + shell_single_quote(output_path) +
        " 2> " + shell_single_quote(error_path);
    log_debug("external command kind=selection-filter spawn command=" + shell_command);
    pending.tokens.clear();
    pending.motion = PendingMotion::None;
    pending.motion_repeat_count = 1;
    pending.repeat_digits.clear();
    def_prog_mode();
    endwin();
    restore_shell_terminal_state();
    int result = std::system(shell_command.c_str());
    reset_prog_mode();
    refresh();
    clearok(stdscr, TRUE);
    log_debug("external command kind=selection-filter exit command=" + shell_command + " status=" + std::to_string(result));

    std::ifstream output_file(output_path, std::ios::binary);
    std::string output_utf8((std::istreambuf_iterator<char>(output_file)), std::istreambuf_iterator<char>());
    output_text = utf8_to_u32(output_utf8);

    std::ifstream stderr_file(error_path, std::ios::binary);
    std::string stderr_text((std::istreambuf_iterator<char>(stderr_file)), std::istreambuf_iterator<char>());

    std::filesystem::remove(input_path);
    std::filesystem::remove(output_path);
    std::filesystem::remove(error_path);

    if (result != 0) {
        error_message = trim_ascii_whitespace(stderr_text);
        if (error_message.empty()) {
            error_message = "filter command failed";
        }
        return false;
    }
    return true;
#else
    (void)command;
    (void)input_text;
    (void)output_text;
    error_message = "selection filters unsupported on this platform";
    return false;
#endif
}

bool parse_grep_selection(
    const std::string &selection,
    std::string &path,
    std::size_t &row,
    std::size_t &column) {
    std::size_t first_colon = selection.find(':');
    if (first_colon == std::string::npos) {
        return false;
    }
    std::size_t second_colon = selection.find(':', first_colon + 1);
    if (second_colon == std::string::npos) {
        return false;
    }
    std::size_t third_colon = selection.find(':', second_colon + 1);
    if (third_colon == std::string::npos) {
        return false;
    }

    path = selection.substr(0, first_colon);
    try {
        std::size_t parsed_row = static_cast<std::size_t>(std::stoul(selection.substr(first_colon + 1, second_colon - first_colon - 1)));
        std::size_t parsed_column =
            static_cast<std::size_t>(std::stoul(selection.substr(second_colon + 1, third_colon - second_colon - 1)));
        row = parsed_row > 0 ? parsed_row - 1 : 0;
        column = parsed_column > 0 ? parsed_column - 1 : 0;
    } catch (const std::exception &) {
        return false;
    }
    return !path.empty();
}

void EditorState::handle_find_file_command() {
    std::string error_message;
    std::optional<std::string> selection =
        run_picker_command(project_file_list_command() + " | fzf", error_message);
    if (!selection) {
        log_debug("find-file canceled/error: " + error_message);
        set_status(error_message);
        return;
    }
    std::filesystem::path resolved = *selection;
    if (resolved.is_relative()) {
        resolved = std::filesystem::current_path() / resolved;
    }
    resolved = resolved.lexically_normal();
    log_debug("find-file resolved path=" + resolved.string());
    handle_edit_command(resolved.string());
}

void EditorState::open_startup_file_picker(const std::optional<std::filesystem::path> &root) {
    std::string error_message;
    std::optional<std::string> selection =
        run_picker_command(project_file_list_command(root) + " | fzf", error_message);
    if (!selection) {
        log_debug("startup picker canceled/error: " + error_message);
        if (!error_message.empty()) {
            set_status(error_message);
        }
        return;
    }

    std::filesystem::path resolved = *selection;
    if (resolved.is_relative()) {
        resolved = (root && !root->empty() ? *root : std::filesystem::current_path()) / resolved;
    }
    resolved = resolved.lexically_normal();
    log_debug("startup picker resolved path=" + resolved.string());

    EditorBuffer *buffer = session.open_file(resolved.string(), true);
    if (!buffer) {
        log_debug("startup picker open failed path=" + resolved.string());
        set_status("Could not open file");
        return;
    }
    windows.set_active_buffer_id(buffer->id);
    sync_active_window_buffer();
    active_buffer_ui();
    set_status("Opened " + resolved.string());
}

void EditorState::handle_grep_command(const std::string &argument) {
    if (argument.empty()) {
        set_status("No grep pattern");
        return;
    }

    std::string error_message;
    std::string command =
        "rg --column --line-number --no-heading --color=never --smart-case " +
        shell_single_quote(argument) + " | fzf";
    std::optional<std::string> selection = run_picker_command(command, error_message);
    if (!selection) {
        log_debug("grep picker canceled/error: " + error_message);
        set_status(error_message);
        return;
    }

    std::string path;
    std::size_t row = 0;
    std::size_t column = 0;
    if (!parse_grep_selection(*selection, path, row, column)) {
        log_debug("grep parse failed selection=[" + *selection + "]");
        set_status("Could not parse grep result");
        return;
    }
    std::filesystem::path resolved = path;
    if (resolved.is_relative()) {
        resolved = std::filesystem::current_path() / resolved;
    }
    path = resolved.lexically_normal().string();
    log_debug("grep resolved path=" + path + " row=" + std::to_string(row) + " column=" + std::to_string(column));

    EditorBuffer *existing = nullptr;
    for (EditorBuffer &buffer : session.buffers()) {
        if (buffer.core.file_path() && *buffer.core.file_path() == path) {
            existing = &buffer;
            break;
        }
    }
    if (existing) {
        if (std::optional<std::size_t> window_id = windows.find_window_showing_buffer(existing->id)) {
            focus_window(*window_id);
        } else {
            show_buffer_in_active_window(existing->id);
        }
        active_core().set_cursor({row, column});
        set_status("Opened " + path);
        return;
    }

    EditorBuffer *buffer = session.open_file(path, true);
    if (!buffer) {
        log_debug("grep open failed path=" + path);
        set_status("Could not open file");
        return;
    }
    windows.set_active_buffer_id(buffer->id);
    sync_active_window_buffer();
    active_buffer_ui();
    active_core().set_cursor({row, column});
    set_status("Opened " + path);
}

void EditorState::execute_filter_command() {
    EditorCore &core = active_core();
    std::optional<Range> selection = displayed_selection_range(windows.active_window_id());
    if (!selection) {
        set_status("No selection");
        enter_normal_mode();
        return;
    }

    std::string command = u32_to_utf8(command_buffer);
    if (command.empty()) {
        set_status("No filter command");
        enter_normal_mode();
        return;
    }

    std::u32string output_text;
    std::string error_message;
    if (!run_filter_command(command, core.read_text(*selection), output_text, error_message)) {
        set_status(error_message);
        enter_normal_mode();
        return;
    }

    if (core.selection_mode() == SelectionMode::Line && !output_text.empty() && output_text.back() != U'\n') {
        output_text.push_back(U'\n');
    }

    core.replace_range(*selection, output_text);
    enter_normal_mode();
    set_status("Selection filtered");
}

void EditorState::execute_sed_command() {
    EditorCore &core = active_core();
    std::optional<Range> selection = core.selection_range();
    if (!selection) {
        set_status("No selection");
        enter_normal_mode();
        return;
    }

    std::string script = u32_to_utf8(command_buffer);
    if (script.empty()) {
        set_status("No substitute command");
        enter_normal_mode();
        return;
    }

    SubstituteCommand substitute;
    std::string error_message;
    if (!parse_substitute_command(script, substitute, error_message)) {
        set_status(error_message);
        enter_normal_mode();
        return;
    }

    std::size_t substitutions = 0;
    if (core.selection_mode() == SelectionMode::Line) {
        Range normalized = normalized_range(*selection);
        std::size_t end_row = normalized.end.column == 0 && normalized.end.row > normalized.start.row
            ? normalized.end.row - 1
            : normalized.end.row;
        substitutions = core.substitute_regex(
            normalized.start.row,
            end_row,
            substitute.pattern,
            substitute.replacement,
            substitute.global,
            error_message);
    } else {
        substitutions = core.substitute_regex_in_range(
            *selection,
            substitute.pattern,
            substitute.replacement,
            substitute.global,
            error_message);
    }
    enter_normal_mode();
    if (!error_message.empty()) {
        set_status(error_message);
        return;
    }
    if (substitutions == 0) {
        set_status("Pattern not found");
        return;
    }
    set_status(count_label(substitutions, "substitution"));
}

enum class NamedEditorCommand {
    Write,
    Quit,
    ForceQuit,
    WriteQuit,
    WriteIfChangedQuit,
    Edit,
    Buffers,
    Buffer,
    NextBuffer,
    PreviousBuffer,
    DeleteBuffer,
    ForceDeleteBuffer,
    ReloadConfig,
    Diagnostics,
    LspStatus,
    TreeSitterStatus,
    LuaCommand,
};

struct NamedEditorCommandInfo {
    std::string_view name;
    std::string_view detail;
    std::string_view completion_text;
    NamedEditorCommand command;
};

constexpr std::array<NamedEditorCommandInfo, 17> kNamedEditorCommands{{
    {"w", "write current buffer", "w", NamedEditorCommand::Write},
    {"q", "quit", "q", NamedEditorCommand::Quit},
    {"q!", "force quit", "q!", NamedEditorCommand::ForceQuit},
    {"wq", "write and quit", "wq", NamedEditorCommand::WriteQuit},
    {"x", "write and quit if modified", "x", NamedEditorCommand::WriteIfChangedQuit},
    {"e", "edit file in active window", "e ", NamedEditorCommand::Edit},
    {"buffers", "list open buffers", "buffers", NamedEditorCommand::Buffers},
    {"buffer", "switch to buffer", "buffer ", NamedEditorCommand::Buffer},
    {"bnext", "next buffer", "bnext", NamedEditorCommand::NextBuffer},
    {"bprev", "previous buffer", "bprev", NamedEditorCommand::PreviousBuffer},
    {"bd", "close current buffer", "bd", NamedEditorCommand::DeleteBuffer},
    {"bd!", "force close current buffer", "bd!", NamedEditorCommand::ForceDeleteBuffer},
    {"reload-config", "reload medit configuration", "reload-config", NamedEditorCommand::ReloadConfig},
    {"diagnostics", "show diagnostics summary", "diagnostics", NamedEditorCommand::Diagnostics},
    {"lsp-status", "show language server status", "lsp-status", NamedEditorCommand::LspStatus},
    {"tree-sitter-status", "show tree-sitter status", "tree-sitter-status", NamedEditorCommand::TreeSitterStatus},
    {"lua-command", "run registered Lua command", "lua-command ", NamedEditorCommand::LuaCommand},
}};

bool parse_substitute_component(
    std::string_view command,
    std::size_t &index,
    char delimiter,
    std::string &component,
    std::string &error_message) {
    while (index < command.size()) {
        char ch = command[index];
        if (ch == delimiter) {
            ++index;
            return true;
        }
        if (ch == '\\' && index + 1 < command.size() &&
            (command[index + 1] == delimiter || command[index + 1] == '\\')) {
            component.push_back(command[index + 1]);
            index += 2;
            continue;
        }
        component.push_back(ch);
        ++index;
    }
    error_message = "unterminated substitute command";
    return false;
}

bool parse_substitute_command(std::string_view command, SubstituteCommand &parsed, std::string &error_message) {
    std::size_t index = 0;
    if (command.starts_with("%s")) {
        parsed.whole_buffer = true;
        index = 2;
    } else if (command.starts_with("s")) {
        index = 1;
    } else {
        return false;
    }

    if (index >= command.size()) {
        error_message = "missing substitute delimiter";
        return false;
    }
    char delimiter = command[index++];
    if (std::isalnum(static_cast<unsigned char>(delimiter)) || std::isspace(static_cast<unsigned char>(delimiter))) {
        error_message = "invalid substitute delimiter";
        return false;
    }

    if (!parse_substitute_component(command, index, delimiter, parsed.pattern, error_message)) {
        return false;
    }
    if (!parse_substitute_component(command, index, delimiter, parsed.replacement, error_message)) {
        return false;
    }

    for (; index < command.size(); ++index) {
        if (command[index] == 'g') {
            parsed.global = true;
            continue;
        }
        error_message = "unsupported substitute flags";
        return false;
    }
    return true;
}

void EditorState::handle_substitute_command(const SubstituteCommand &command) {
    EditorCore &core = active_core();
    std::size_t start_row = command.whole_buffer ? 0 : core.cursor().row;
    std::size_t end_row = command.whole_buffer ? core.line_count() - 1 : core.cursor().row;
    std::string error_message;
    std::size_t substitutions =
        core.substitute_regex(start_row, end_row, command.pattern, command.replacement, command.global, error_message);
    if (!error_message.empty()) {
        set_status(error_message);
        return;
    }
    if (substitutions == 0) {
        set_status("Pattern not found");
        return;
    }
    set_status(count_label(substitutions, "substitution"));
}

void EditorState::handle_lua_command(const std::string &argument) {
    if (argument.empty()) {
        set_status("No Lua command name");
        return;
    }

    std::string name = argument;
    std::string command_argument;
    std::size_t separator = argument.find_first_of(" \t");
    if (separator != std::string::npos) {
        name = argument.substr(0, separator);
        std::size_t argument_start = argument.find_first_not_of(" \t", separator);
        command_argument = argument_start == std::string::npos ? std::string() : argument.substr(argument_start);
    }

    std::string error_message;
    if (!lua.execute_command(*this, name, command_argument, error_message)) {
        set_status(error_message);
        return;
    }
    if (!apply_pending_config_reload()) {
        return;
    }
    if (status_message.empty() || status_message == ":") {
        set_status("Lua command: " + name);
    }
}

PopupMenuItems command_completion_items() {
    PopupMenuItems items;
    items.reserve(kNamedEditorCommands.size() + 2);
    for (const NamedEditorCommandInfo &command : kNamedEditorCommands) {
        items.push_back(
            {std::string(command.name), std::string(command.detail), std::string(command.completion_text), std::nullopt});
    }
    items.push_back({"s", "substitute on current line", "s/", std::nullopt});
    items.push_back({"%s", "substitute in whole buffer", "%s/", std::nullopt});
    return items;
}

std::vector<std::string> lua_command_names(const LuaRuntime &lua) {
    return lua.registered_commands();
}

void append_lua_command_completion_items(PopupMenuItems &items, const LuaRuntime &lua) {
    std::vector<std::string> commands = lua_command_names(lua);
    items.reserve(items.size() + commands.size());
    for (const std::string &name : commands) {
        items.push_back({name, "Lua command", name, std::nullopt});
    }
}

std::optional<NamedEditorCommand> named_editor_command_from_verb(std::string_view verb) {
    auto found = std::find_if(
        kNamedEditorCommands.begin(),
        kNamedEditorCommands.end(),
        [verb](const NamedEditorCommandInfo &command) { return command.name == verb; });
    if (found == kNamedEditorCommands.end()) {
        return std::nullopt;
    }
    return found->command;
}

void execute_named_editor_command(
    EditorState &state,
    NamedEditorCommand command,
    const std::string &argument) {
    switch (command) {
        case NamedEditorCommand::Write:
            state.handle_write_command(argument);
            break;
        case NamedEditorCommand::Quit:
            state.handle_quit_command(false);
            break;
        case NamedEditorCommand::ForceQuit:
            state.handle_quit_command(true);
            break;
        case NamedEditorCommand::WriteQuit:
            state.handle_write_quit_command(argument);
            break;
        case NamedEditorCommand::WriteIfChangedQuit:
            state.handle_write_quit_command(argument);
            break;
        case NamedEditorCommand::Edit:
            state.handle_edit_command(argument);
            break;
        case NamedEditorCommand::Buffers:
            state.set_status(buffers_summary(state));
            break;
        case NamedEditorCommand::Buffer:
            state.handle_buffer_switch_command(argument);
            break;
        case NamedEditorCommand::NextBuffer:
            state.session.next_buffer();
            state.set_status(prefixed_message("Switched to ", state.active_core().display_file_name()));
            break;
        case NamedEditorCommand::PreviousBuffer:
            state.session.previous_buffer();
            state.set_status(prefixed_message("Switched to ", state.active_core().display_file_name()));
            break;
        case NamedEditorCommand::DeleteBuffer:
            state.handle_buffer_delete_command(false);
            break;
        case NamedEditorCommand::ForceDeleteBuffer:
            state.handle_buffer_delete_command(true);
            break;
        case NamedEditorCommand::ReloadConfig: {
            std::string error_message;
            if (state.reload_editor_configuration(error_message)) {
                state.set_status("Reloaded config");
            } else {
                state.set_status("Config reload failed: " + error_message);
            }
            break;
        }
        case NamedEditorCommand::Diagnostics:
            state.show_diagnostics_summary();
            break;
        case NamedEditorCommand::LspStatus:
            state.show_lsp_status();
            break;
        case NamedEditorCommand::TreeSitterStatus:
            state.show_tree_sitter_status();
            break;
        case NamedEditorCommand::LuaCommand:
            state.handle_lua_command(argument);
            break;
    }
}

void EditorState::show_command_completion() {
    if (mode != Mode::Command || command_prompt_kind != CommandPromptKind::EditorCommand) {
        return;
    }
    std::string command = u32_to_utf8(command_buffer);
    if (std::optional<EditFileCompletionResult> completion = complete_edit_file_command(command)) {
        if (completion->items.empty()) {
            set_status("No file matches");
            return;
        }
        show_menu_popup(
            *this,
            "Files",
            std::move(completion->items),
            PopupApplyTarget::CommandBuffer,
            utf8_to_u32(completion->initial_filter),
            PopupFilterMode::PrefixLabelOnly);
        return;
    }
    constexpr std::string_view lua_prefix = "lua-command ";
    if (command.starts_with(lua_prefix)) {
        PopupMenuItems items;
        std::string filter = command.substr(lua_prefix.size());
        for (const std::string &name : lua_command_names(lua)) {
            items.push_back({name, "Lua command", "lua-command " + name, std::nullopt});
        }
        if (items.empty()) {
            set_status("No Lua commands");
            return;
        }
        show_menu_popup(
            *this,
            "Lua Commands",
            std::move(items),
            PopupApplyTarget::CommandBuffer,
            utf8_to_u32(filter),
            PopupFilterMode::PrefixLabelOnly);
        return;
    }
    if (command.contains(' ') || command.contains('\t')) {
        set_status("Complete the command name only");
        return;
    }
    show_menu_popup(
        *this,
        "Commands",
        [&]() {
            PopupMenuItems items = command_completion_items();
            append_lua_command_completion_items(items, lua);
            return items;
        }(),
        PopupApplyTarget::CommandBuffer,
        command_buffer,
        PopupFilterMode::PrefixLabelOnly);
}

void EditorState::execute_command() {
    if (command_prompt_kind == CommandPromptKind::FilterSelection) {
        execute_filter_command();
        return;
    }
    if (command_prompt_kind == CommandPromptKind::SedSelection) {
        execute_sed_command();
        return;
    }

    std::u32string command_text = command_buffer;
    std::string command = u32_to_utf8(command_text);
    log_debug("ex command=" + command);
    std::istringstream parser(command);
    std::string verb;
    parser >> verb;

    std::string argument;
    std::getline(parser, argument);
    if (!argument.empty() && argument.front() == ' ') {
        argument.erase(argument.begin());
    }

    if (verb.empty()) {
        enter_normal_mode();
        return;
    }
    add_prompt_history_entry(command_text);
    SubstituteCommand substitute;
    std::string substitute_error;
    if (parse_substitute_command(command, substitute, substitute_error)) {
        handle_substitute_command(substitute);
    } else if (!substitute_error.empty()) {
        set_status(substitute_error);
    } else if (std::all_of(verb.begin(), verb.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
        handle_goto_line_command(verb);
    } else if (std::optional<NamedEditorCommand> named = named_editor_command_from_verb(verb)) {
        execute_named_editor_command(*this, *named, argument);
    } else if (argument.empty()) {
        std::string error_message;
        if (lua.execute_command(*this, verb, std::string(), error_message)) {
            if (!apply_pending_config_reload()) {
                enter_normal_mode(true);
                return;
            }
            if (status_message.empty() || status_message == ":") {
                set_status("Lua command: " + verb);
            }
        } else {
            if (error_message == "No such Lua command: " + verb) {
                set_status("Unknown command: " + verb);
            } else {
                set_status(error_message);
            }
        }
    } else if (std::vector<std::string> commands = lua_command_names(lua); std::binary_search(commands.begin(), commands.end(), verb)) {
        std::string error_message;
        if (lua.execute_command(*this, verb, argument, error_message)) {
            if (!apply_pending_config_reload()) {
                enter_normal_mode(true);
                return;
            }
            if (status_message.empty() || status_message == ":") {
                set_status("Lua command: " + verb);
            }
        } else {
            set_status(error_message);
        }
    } else {
        set_status("Unknown command: " + verb);
    }
    enter_normal_mode(true);
}
