#include "lua_runtime.hpp"

#include "editor_internal.hpp"
#include "logger.hpp"
#include "process_utils.hpp"
#include "string_utils.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <deque>
#include <fstream>
#include <filesystem>
#include <map>
#include <mutex>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

import theme;

#if defined(MEDIT_HAS_LUA) && MEDIT_HAS_LUA
extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}
#endif

#if defined(__unix__) || defined(__APPLE__)
#include <csignal>
#include <sys/types.h>
#include <unistd.h>
#include <sys/wait.h>
#endif

namespace {

const char *event_name(EditorEventType type) {
    switch (type) {
        case EditorEventType::DocumentOpened: return "document_opened";
        case EditorEventType::DocumentChanged: return "document_changed";
        case EditorEventType::DocumentSaved: return "document_saved";
        case EditorEventType::DocumentClosed: return "document_closed";
        case EditorEventType::CursorMoved: return "cursor_moved";
        case EditorEventType::DiagnosticsChanged: return "diagnostics_changed";
        case EditorEventType::AnnotationsChanged: return "annotations_changed";
    }
    return "unknown";
}

#if defined(MEDIT_HAS_LUA) && MEDIT_HAS_LUA

void push_position(lua_State *lua, Position position) {
    lua_createtable(lua, 0, 2);
    lua_pushinteger(lua, static_cast<lua_Integer>(position.row));
    lua_setfield(lua, -2, "row");
    lua_pushinteger(lua, static_cast<lua_Integer>(position.column));
    lua_setfield(lua, -2, "column");
}

Position lua_check_position(lua_State *lua, int index) {
    luaL_checktype(lua, index, LUA_TTABLE);
    lua_getfield(lua, index, "row");
    lua_getfield(lua, index, "column");
    if (!lua_isinteger(lua, -2) || !lua_isinteger(lua, -1)) {
        lua_pop(lua, 2);
        luaL_error(lua, "position must contain integer row and column");
    }
    lua_Integer row = lua_tointeger(lua, -2);
    lua_Integer column = lua_tointeger(lua, -1);
    lua_pop(lua, 2);
    if (row < 0 || column < 0) {
        luaL_error(lua, "position row and column must be non-negative");
    }
    return {static_cast<std::size_t>(row), static_cast<std::size_t>(column)};
}

Range lua_check_range(lua_State *lua, int index) {
    luaL_checktype(lua, index, LUA_TTABLE);
    lua_getfield(lua, index, "start");
    lua_getfield(lua, index, "end");
    if (!lua_istable(lua, -2) || !lua_istable(lua, -1)) {
        lua_pop(lua, 2);
        luaL_error(lua, "range must contain start and end positions");
    }
    Position start = lua_check_position(lua, -2);
    Position end = lua_check_position(lua, -1);
    lua_pop(lua, 2);
    return {start, end};
}

void push_range(lua_State *lua, const Range &range) {
    lua_createtable(lua, 0, 2);
    push_position(lua, range.start);
    lua_setfield(lua, -2, "start");
    push_position(lua, range.end);
    lua_setfield(lua, -2, "end");
}

EditorBuffer *lua_target_buffer(EditorState &state, lua_State *lua, int index) {
    if (lua_gettop(lua) < index || lua_isnil(lua, index)) {
        return &state.active_buffer();
    }
    lua_Integer raw_buffer_id = luaL_checkinteger(lua, index);
    if (raw_buffer_id <= 0) {
        luaL_error(lua, "buffer id must be positive");
    }
    EditorBuffer *buffer = state.session.find_buffer_by_id(static_cast<std::size_t>(raw_buffer_id));
    if (buffer == nullptr) {
        luaL_error(lua, "buffer not found");
    }
    return buffer;
}

AnnotationSeverity lua_check_annotation_severity(lua_State *lua, int index) {
    if (lua_isnil(lua, index)) {
        return AnnotationSeverity::Info;
    }
    std::string severity = luaL_checkstring(lua, index);
    if (severity == "info") {
        return AnnotationSeverity::Info;
    }
    if (severity == "warning") {
        return AnnotationSeverity::Warning;
    }
    if (severity == "error") {
        return AnnotationSeverity::Error;
    }
    luaL_error(lua, "severity must be info, warning, or error");
    return AnnotationSeverity::Info;
}

TextStyle lua_check_text_style(lua_State *lua, int index) {
    luaL_checktype(lua, index, LUA_TTABLE);
    TextStyle style;

    lua_getfield(lua, index, "foreground");
    if (!lua_isnil(lua, -1)) {
        std::optional<short> color = try_parse_theme_color(luaL_checkstring(lua, -1));
        if (!color) {
            lua_pop(lua, 1);
            luaL_error(lua, "invalid foreground color");
        }
        style.foreground = *color;
    }
    lua_pop(lua, 1);

    lua_getfield(lua, index, "background");
    if (!lua_isnil(lua, -1)) {
        std::optional<short> color = try_parse_theme_color(luaL_checkstring(lua, -1));
        if (!color) {
            lua_pop(lua, 1);
            luaL_error(lua, "invalid background color");
        }
        style.background = *color;
    }
    lua_pop(lua, 1);

    lua_getfield(lua, index, "bold");
    if (!lua_isnil(lua, -1)) {
        luaL_checktype(lua, -1, LUA_TBOOLEAN);
        style.bold = lua_toboolean(lua, -1);
    }
    lua_pop(lua, 1);

    lua_getfield(lua, index, "underline");
    if (!lua_isnil(lua, -1)) {
        luaL_checktype(lua, -1, LUA_TBOOLEAN);
        style.underline = lua_toboolean(lua, -1);
    }
    lua_pop(lua, 1);

    lua_getfield(lua, index, "reverse");
    if (!lua_isnil(lua, -1)) {
        luaL_checktype(lua, -1, LUA_TBOOLEAN);
        style.reverse = lua_toboolean(lua, -1);
    }
    lua_pop(lua, 1);
    return style;
}

InlineAnnotations lua_check_line_annotations(lua_State *lua, EditorBuffer &buffer, int index) {
    luaL_checktype(lua, index, LUA_TTABLE);
    InlineAnnotations annotations;
    const EditorCore &core = buffer.core;
    lua_pushnil(lua);
    while (lua_next(lua, index) != 0) {
        luaL_checktype(lua, -1, LUA_TTABLE);
        InlineAnnotation annotation;

        lua_getfield(lua, -1, "line");
        lua_Integer raw_line = luaL_checkinteger(lua, -1);
        lua_pop(lua, 1);
        if (raw_line < 0) {
            luaL_error(lua, "line must be non-negative");
        }
        std::size_t line = static_cast<std::size_t>(raw_line);
        if (line >= core.line_count()) {
            luaL_error(lua, "line out of range");
        }
        annotation.range = core.line_range(line);

        lua_getfield(lua, -1, "text");
        std::string text = luaL_checkstring(lua, -1);
        lua_pop(lua, 1);
        annotation.text = utf8_to_u32(text);

        lua_getfield(lua, -1, "severity");
        annotation.severity = lua_check_annotation_severity(lua, -1);
        lua_pop(lua, 1);

        lua_getfield(lua, -1, "source");
        if (!lua_isnil(lua, -1)) {
            annotation.source = luaL_checkstring(lua, -1);
        } else {
            annotation.source = "lua";
        }
        lua_pop(lua, 1);

        lua_getfield(lua, -1, "style");
        if (!lua_isnil(lua, -1)) {
            annotation.style_override = lua_check_text_style(lua, -1);
        }
        lua_pop(lua, 1);

        annotations.push_back(std::move(annotation));
        lua_pop(lua, 1);
    }
    return annotations;
}

void push_editor_event(lua_State *lua, const EditorEvent &event, std::optional<std::size_t> buffer_id) {
    lua_createtable(lua, 0, 7);
    lua_pushstring(lua, event_name(event.type));
    lua_setfield(lua, -2, "type");
    lua_pushlstring(lua, event.document_uri.data(), event.document_uri.size());
    lua_setfield(lua, -2, "document_uri");
    lua_pushinteger(lua, static_cast<lua_Integer>(event.document_version));
    lua_setfield(lua, -2, "document_version");
    if (buffer_id) {
        lua_pushinteger(lua, static_cast<lua_Integer>(*buffer_id));
    } else {
        lua_pushnil(lua);
    }
    lua_setfield(lua, -2, "buffer_id");
    push_position(lua, event.cursor);
    lua_setfield(lua, -2, "cursor");
    if (event.range) {
        push_range(lua, *event.range);
    } else {
        lua_pushnil(lua);
    }
    lua_setfield(lua, -2, "range");
    std::string text = u32_to_utf8(event.text);
    lua_pushlstring(lua, text.data(), text.size());
    lua_setfield(lua, -2, "text");
}

class ScopedLuaEditorState {
  public:
    ScopedLuaEditorState(EditorState *&slot, EditorState &state) : slot_(slot), previous_(slot) {
        slot_ = &state;
    }

    ~ScopedLuaEditorState() {
        slot_ = previous_;
    }

  private:
    EditorState *&slot_;
    EditorState *previous_ = nullptr;
};

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

std::optional<std::string> path_token_under_cursor(const EditorCore &core) {
    Position cursor = core.cursor();
    if (cursor.row >= core.line_count()) {
        return std::nullopt;
    }
    const std::u32string &line = core.lines()[cursor.row];
    if (line.empty()) {
        return std::nullopt;
    }

    auto is_path_char = [](char32_t ch) {
        return (ch >= U'a' && ch <= U'z') || (ch >= U'A' && ch <= U'Z') || (ch >= U'0' && ch <= U'9') ||
            ch == U'_' || ch == U'-' || ch == U'.' || ch == U'/' || ch == U'\\';
    };

    std::size_t column = cursor.column;
    if (column >= line.size()) {
        if (column == 0) {
            return std::nullopt;
        }
        column = line.size() - 1;
    } else if (!is_path_char(line[column]) && column > 0 && is_path_char(line[column - 1])) {
        --column;
    }

    if (!is_path_char(line[column])) {
        return std::nullopt;
    }

    std::size_t start = column;
    while (start > 0 && is_path_char(line[start - 1])) {
        --start;
    }
    std::size_t end = column + 1;
    while (end < line.size() && is_path_char(line[end])) {
        ++end;
    }

    std::string token = u32_to_utf8(line.substr(start, end - start));
    while (!token.empty() && (token.front() == '"' || token.front() == '\'')) {
        token.erase(token.begin());
    }
    while (!token.empty() && (token.back() == '"' || token.back() == '\'' || token.back() == ',' || token.back() == ';')) {
        token.pop_back();
    }
    if (token.empty()) {
        return std::nullopt;
    }
    return token;
}

std::filesystem::path workspace_root_for_core(const EditorState &state, const EditorCore &core) {
    if (const LspServerConfig *server = matching_lsp_server(state.config, core.file_path())) {
        return infer_workspace_root(*server, core.file_path());
    }
    if (core.file_path()) {
        return std::filesystem::path(*core.file_path()).parent_path();
    }
    return std::filesystem::current_path();
}

std::optional<std::string> resolve_ai_command(const EditorConfig &config) {
    if (config.ai_command && !config.ai_command->empty()) {
        return config.ai_command;
    }
    if (executable_exists("medit-ai")) {
        return std::string("medit-ai");
    }
    if (executable_exists("./tools/medit_ai.py")) {
        return std::string("./tools/medit_ai.py");
    }
    if (executable_exists("tools/medit_ai.py")) {
        return std::string("tools/medit_ai.py");
    }
    return std::nullopt;
}

std::string resolve_ai_provider(const EditorConfig &config) {
    if (config.ai_provider && !config.ai_provider->empty()) {
        return *config.ai_provider;
    }
    const char *llm_provider = std::getenv("LLM_PROVIDER");
    if (llm_provider != nullptr) {
        std::string normalized = ascii_lowercase(llm_provider);
        if (normalized == "openai" || normalized == "mistral") {
            return normalized;
        }
    }
    const char *openai_key = std::getenv("OPENAI_API_KEY");
    if (openai_key != nullptr && *openai_key != '\0') {
        return "openai";
    }
    const char *mistral_key = std::getenv("MISTRAL_API_KEY");
    if (mistral_key != nullptr && *mistral_key != '\0') {
        return "mistral";
    }
    return "openai";
}

std::string resolve_ai_model(const EditorConfig &config, std::string_view provider) {
    if (config.ai_model && !config.ai_model->empty()) {
        return *config.ai_model;
    }
    const char *llm_model = std::getenv("LLM_MODEL");
    if (llm_model != nullptr && *llm_model != '\0') {
        return llm_model;
    }
    if (provider == "mistral") {
        const char *mistral_model = std::getenv("MISTRAL_MODEL");
        if (mistral_model != nullptr && *mistral_model != '\0') {
            return mistral_model;
        }
        return "mistral-small-latest";
    }
    const char *openai_model = std::getenv("OPENAI_MODEL");
    if (openai_model != nullptr && *openai_model != '\0') {
        return openai_model;
    }
    return "gpt-5-nano";
}

Range full_buffer_range(const EditorCore &core) {
    const Lines &lines = core.lines();
    if (lines.empty()) {
        return {{0, 0}, {0, 0}};
    }
    return {{0, 0}, {lines.size() - 1, lines.back().size()}};
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

std::optional<EditorBufferKind> parse_buffer_kind(std::string_view kind_name) {
    if (kind_name == "file") {
        return EditorBufferKind::File;
    }
    if (kind_name == "scratch") {
        return EditorBufferKind::Scratch;
    }
    if (kind_name == "output") {
        return EditorBufferKind::Output;
    }
    if (kind_name == "list") {
        return EditorBufferKind::List;
    }
    return std::nullopt;
}

struct AsyncJobEvent {
    enum class Type {
        Output,
        Exit,
    };

    Type type = Type::Output;
    std::size_t job_id = 0;
    std::string text;
    int exit_code = -1;
};

struct AsyncJobQueue {
    std::mutex mutex;
    std::deque<AsyncJobEvent> events;
};

struct ProcessEvent {
    enum class Type {
        Stdout,
        Stderr,
        Exit,
    };

    Type type = Type::Stdout;
    std::size_t process_id = 0;
    std::string text;
    int exit_code = -1;
};

struct ProcessEventQueue {
    std::mutex mutex;
    std::deque<ProcessEvent> events;
};

void push_async_job_event(
    const std::shared_ptr<AsyncJobQueue> &queue,
    AsyncJobEvent::Type type,
    std::size_t job_id,
    std::string text = {},
    int exit_code = -1) {
    std::lock_guard<std::mutex> lock(queue->mutex);
    queue->events.push_back({type, job_id, std::move(text), exit_code});
}

void push_process_event(
    const std::shared_ptr<ProcessEventQueue> &queue,
    ProcessEvent::Type type,
    std::size_t process_id,
    std::string text = {},
    int exit_code = -1) {
    std::lock_guard<std::mutex> lock(queue->mutex);
    queue->events.push_back({type, process_id, std::move(text), exit_code});
}

int decode_process_exit_code(int status) {
#if defined(__unix__) || defined(__APPLE__)
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
#endif
    return status;
}

#if defined(__unix__) || defined(__APPLE__)
void forward_process_stream(
    std::shared_ptr<ProcessEventQueue> queue,
    std::size_t process_id,
    int fd,
    ProcessEvent::Type type) {
    std::array<char, 4096> buffer{};
    for (;;) {
        ssize_t bytes = read(fd, buffer.data(), buffer.size());
        if (bytes > 0) {
            push_process_event(queue, type, process_id, std::string(buffer.data(), static_cast<std::size_t>(bytes)));
            continue;
        }
        if (bytes < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    close(fd);
}

void wait_for_process_exit(
    std::shared_ptr<ProcessEventQueue> queue,
    std::size_t process_id,
    pid_t pid) {
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        push_process_event(queue, ProcessEvent::Type::Exit, process_id, {}, -1);
        return;
    }
    push_process_event(queue, ProcessEvent::Type::Exit, process_id, {}, decode_process_exit_code(status));
}

bool spawn_shell_process(
    const std::shared_ptr<ProcessEventQueue> &queue,
    std::size_t process_id,
    const std::string &command,
    pid_t &pid,
    int &stdin_fd,
    std::string &error_message) {
    int stdin_pipe[2] = {-1, -1};
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
        error_message = "could not create process pipes";
        if (stdin_pipe[0] >= 0) close(stdin_pipe[0]);
        if (stdin_pipe[1] >= 0) close(stdin_pipe[1]);
        if (stdout_pipe[0] >= 0) close(stdout_pipe[0]);
        if (stdout_pipe[1] >= 0) close(stdout_pipe[1]);
        if (stderr_pipe[0] >= 0) close(stderr_pipe[0]);
        if (stderr_pipe[1] >= 0) close(stderr_pipe[1]);
        return false;
    }

    pid = fork();
    if (pid < 0) {
        error_message = "could not fork process";
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);
        return false;
    }

    if (pid == 0) {
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);

        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);

        execl("/bin/sh", "sh", "-lc", command.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }

    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);
    stdin_fd = stdin_pipe[1];

    std::thread(forward_process_stream, queue, process_id, stdout_pipe[0], ProcessEvent::Type::Stdout).detach();
    std::thread(forward_process_stream, queue, process_id, stderr_pipe[0], ProcessEvent::Type::Stderr).detach();
    std::thread(wait_for_process_exit, queue, process_id, pid).detach();
    return true;
}
#endif

void run_async_shell_command(
    std::shared_ptr<AsyncJobQueue> queue,
    std::size_t job_id,
    std::string command) {
    std::string shell_command = command + " 2>&1";
    FILE *pipe = popen(shell_command.c_str(), "r");
    if (pipe == nullptr) {
        push_async_job_event(queue, AsyncJobEvent::Type::Exit, job_id, "failed to start process\n", -1);
        return;
    }

    char buffer[4096];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        push_async_job_event(queue, AsyncJobEvent::Type::Output, job_id, std::string(buffer));
    }

    int status = pclose(pipe);
    push_async_job_event(queue, AsyncJobEvent::Type::Exit, job_id, {}, decode_process_exit_code(status));
}

#endif

}  // namespace

struct LuaRuntime::Impl {
    bool initialized = false;

#if defined(MEDIT_HAS_LUA) && MEDIT_HAS_LUA
    lua_State *lua = nullptr;
    EditorState *current_state = nullptr;
    struct RegisteredLuaCommand {
        int ref = LUA_NOREF;
        LuaCommandInfo info;
    };
    std::map<std::string, RegisteredLuaCommand> commands;
    std::map<std::string, std::string> command_aliases;
    std::map<std::string, std::vector<int>> event_refs;
    std::map<std::string, int> health_check_refs;
    struct AsyncJobInfo {
        std::size_t id = 0;
        std::string command;
        std::optional<std::size_t> buffer_id;
        int on_exit_ref = LUA_NOREF;
        bool running = false;
        int exit_code = -1;
    };
    struct ProcessInfo {
        std::size_t id = 0;
        std::string command;
        std::optional<std::size_t> buffer_id;
        int on_stdout_ref = LUA_NOREF;
        int on_stderr_ref = LUA_NOREF;
        int on_exit_ref = LUA_NOREF;
        bool running = false;
        int exit_code = -1;
#if defined(__unix__) || defined(__APPLE__)
        pid_t pid = -1;
        int stdin_fd = -1;
#endif
    };
    std::shared_ptr<AsyncJobQueue> async_job_queue = std::make_shared<AsyncJobQueue>();
    std::shared_ptr<ProcessEventQueue> process_event_queue = std::make_shared<ProcessEventQueue>();
    std::map<std::size_t, AsyncJobInfo> async_jobs;
    std::map<std::size_t, ProcessInfo> processes;
    std::size_t next_async_job_id = 1;
    std::size_t next_process_id = 1;

    static LuaRuntime::Impl *from_upvalue(lua_State *lua_state) {
        void *raw = lua_touserdata(lua_state, lua_upvalueindex(1));
        return static_cast<LuaRuntime::Impl *>(raw);
    }

    void unregister_command_aliases(const std::string &name) {
        for (auto it = command_aliases.begin(); it != command_aliases.end();) {
            if (it->second == name) {
                it = command_aliases.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::optional<std::string> resolve_command_name(std::string_view name) const {
        std::string key(name);
        if (commands.contains(key)) {
            return key;
        }
        auto found = command_aliases.find(key);
        if (found != command_aliases.end()) {
            return found->second;
        }
        return std::nullopt;
    }

    bool with_current_state(lua_State *lua_state) const {
        if (current_state != nullptr) {
            return true;
        }
        lua_pushstring(lua_state, "medit API unavailable outside editor callback");
        return false;
    }

    void close_process_stdin(ProcessInfo &process) {
#if defined(__unix__) || defined(__APPLE__)
        if (process.stdin_fd >= 0) {
            close(process.stdin_fd);
            process.stdin_fd = -1;
        }
#else
        (void)process;
#endif
    }

    void stop_process(ProcessInfo &process) {
#if defined(__unix__) || defined(__APPLE__)
        close_process_stdin(process);
        if (process.pid > 0 && process.running) {
            kill(process.pid, SIGTERM);
        }
#else
        (void)process;
#endif
    }

    void release_process_refs(ProcessInfo &process) {
        if (lua == nullptr) {
            process.on_stdout_ref = LUA_NOREF;
            process.on_stderr_ref = LUA_NOREF;
            process.on_exit_ref = LUA_NOREF;
            return;
        }
        for (int *ref : {&process.on_stdout_ref, &process.on_stderr_ref, &process.on_exit_ref}) {
            if (*ref != LUA_NOREF) {
                luaL_unref(lua, LUA_REGISTRYINDEX, *ref);
                *ref = LUA_NOREF;
            }
        }
    }

    static int lua_status(lua_State *lua_state) {
        LuaRuntime::Impl *impl = from_upvalue(lua_state);
        if (!impl->with_current_state(lua_state)) {
            return lua_error(lua_state);
        }

        EditorState &state = *impl->current_state;
        lua_createtable(lua_state, 0, 6);
        lua_pushinteger(lua_state, static_cast<lua_Integer>(state.active_buffer().id));
        lua_setfield(lua_state, -2, "active_buffer_id");
        lua_pushinteger(lua_state, static_cast<lua_Integer>(state.windows.active_window_id()));
        lua_setfield(lua_state, -2, "active_window_id");
        lua_pushinteger(lua_state, static_cast<lua_Integer>(state.session.buffer_count()));
        lua_setfield(lua_state, -2, "buffer_count");
        lua_pushinteger(lua_state, static_cast<lua_Integer>(state.windows.window_count()));
        lua_setfield(lua_state, -2, "window_count");
        lua_pushstring(lua_state, mode_name(state.mode).c_str());
        lua_setfield(lua_state, -2, "mode");
        lua_pushstring(lua_state, state.status_message.c_str());
        lua_setfield(lua_state, -2, "status_message");
        return 1;
    }

    static int lua_set_status(lua_State *lua_state) {
        LuaRuntime::Impl *impl = from_upvalue(lua_state);
        if (!impl->with_current_state(lua_state)) {
            return lua_error(lua_state);
        }

        const char *message = luaL_checkstring(lua_state, 1);
        impl->current_state->set_status(message);
        return 0;
    }

    static int lua_current_working_directory(lua_State *lua_state) {
        std::string cwd = std::filesystem::current_path().string();
        lua_pushlstring(lua_state, cwd.data(), cwd.size());
        return 1;
    }

    static int lua_current_file_path(lua_State *lua_state) {
        LuaRuntime::Impl *impl = from_upvalue(lua_state);
        if (!impl->with_current_state(lua_state)) {
            return lua_error(lua_state);
        }

        if (const std::optional<std::string> &path = impl->current_state->active_core().file_path(); path && !path->empty()) {
            lua_pushlstring(lua_state, path->data(), path->size());
        } else {
            lua_pushnil(lua_state);
        }
        return 1;
    }

    static int lua_workspace_root(lua_State *lua_state) {
        LuaRuntime::Impl *impl = from_upvalue(lua_state);
        if (!impl->with_current_state(lua_state)) {
            return lua_error(lua_state);
        }

        std::filesystem::path root = workspace_root_for_core(*impl->current_state, impl->current_state->active_core());
        if (root.empty()) {
            lua_pushnil(lua_state);
        } else {
            std::string text = root.string();
            lua_pushlstring(lua_state, text.data(), text.size());
        }
        return 1;
    }

    static int lua_token_under_cursor(lua_State *lua_state) {
        LuaRuntime::Impl *impl = from_upvalue(lua_state);
        if (!impl->with_current_state(lua_state)) {
            return lua_error(lua_state);
        }

        std::optional<std::string> token = path_token_under_cursor(impl->current_state->active_core());
        if (!token) {
            lua_pushnil(lua_state);
        } else {
            lua_pushlstring(lua_state, token->data(), token->size());
        }
        return 1;
    }

    static int lua_get_cursor(lua_State *lua_state) {
        LuaRuntime::Impl *impl = from_upvalue(lua_state);
        if (!impl->with_current_state(lua_state)) {
            return lua_error(lua_state);
        }

        push_position(lua_state, impl->current_state->displayed_cursor(impl->current_state->windows.active_window_id()));
        return 1;
    }

    static int lua_get_selection(lua_State *lua_state) {
        LuaRuntime::Impl *impl = from_upvalue(lua_state);
        if (!impl->with_current_state(lua_state)) {
            return lua_error(lua_state);
        }

        std::optional<EditorState::CommandSelectionSnapshot> snapshot =
            impl->current_state->selection_snapshot_for_commands();
        if (!snapshot) {
            lua_pushnil(lua_state);
            return 1;
        }
        push_range(lua_state, snapshot->range);
        return 1;
    }

    static int lua_get_selection_text(lua_State *lua_state) {
        LuaRuntime::Impl *impl = from_upvalue(lua_state);
        if (!impl->with_current_state(lua_state)) {
            return lua_error(lua_state);
        }

        std::optional<std::u32string> text = impl->current_state->selection_text_for_commands();
        if (!text) {
            lua_pushnil(lua_state);
            return 1;
        }
        std::string text_utf8 = u32_to_utf8(*text);
        lua_pushlstring(lua_state, text_utf8.data(), text_utf8.size());
        return 1;
    }

    static int lua_get_buffer_text(lua_State *lua_state) {
        LuaRuntime::Impl *impl = from_upvalue(lua_state);
        if (!impl->with_current_state(lua_state)) {
            return lua_error(lua_state);
        }

        std::string text = buffer_text_utf8(impl->current_state->active_buffer());
        lua_pushlstring(lua_state, text.data(), text.size());
        return 1;
    }

    static int lua_get_line_text(lua_State *lua_state) {
        LuaRuntime::Impl *impl = from_upvalue(lua_state);
        if (!impl->with_current_state(lua_state)) {
            return lua_error(lua_state);
        }

        lua_Integer row = luaL_checkinteger(lua_state, 1);
        if (row < 0) {
            return luaL_error(lua_state, "row must be non-negative");
        }

        const EditorCore &core = impl->current_state->active_core();
        std::size_t row_index = static_cast<std::size_t>(row);
        if (row_index >= core.line_count()) {
            return luaL_error(lua_state, "row out of range");
        }

        const std::u32string &line = core.lines()[row_index];
        std::string text = u32_to_utf8(line);
        lua_pushlstring(lua_state, text.data(), text.size());
        return 1;
    }

    static int lua_set_line_annotations(lua_State *lua_state) {
        LuaRuntime::Impl *impl = from_upvalue(lua_state);
        if (!impl->with_current_state(lua_state)) {
            return lua_error(lua_state);
        }

        EditorBuffer *buffer = lua_target_buffer(*impl->current_state, lua_state, 2);
        InlineAnnotations annotations = lua_check_line_annotations(lua_state, *buffer, 1);
        buffer->core.set_lua_annotations(std::move(annotations));
        return 0;
    }

    static int lua_clear_line_annotations(lua_State *lua_state) {
        LuaRuntime::Impl *impl = from_upvalue(lua_state);
        if (!impl->with_current_state(lua_state)) {
            return lua_error(lua_state);
        }

        EditorBuffer *buffer = lua_target_buffer(*impl->current_state, lua_state, 1);
        buffer->core.clear_lua_annotations();
        return 0;
    }

    static int lua_get_text(lua_State *lua_state) {
        LuaRuntime::Impl *impl = from_upvalue(lua_state);
        if (!impl->with_current_state(lua_state)) {
            return lua_error(lua_state);
        }

        Range range = lua_check_range(lua_state, 1);
        std::string text = u32_to_utf8(impl->current_state->active_core().read_text(range));
        lua_pushlstring(lua_state, text.data(), text.size());
        return 1;
    }

    static int lua_set_cursor(lua_State *lua_state) {
        LuaRuntime::Impl *impl = from_upvalue(lua_state);
        if (!impl->with_current_state(lua_state)) {
            return lua_error(lua_state);
        }

        Position position = lua_check_position(lua_state, 1);
        impl->current_state->active_core().set_cursor(position);
        return 0;
    }

    static int lua_replace_selection(lua_State *lua_state) {
        LuaRuntime::Impl *impl = from_upvalue(lua_state);
        if (!impl->with_current_state(lua_state)) {
            return lua_error(lua_state);
        }

        if (!impl->current_state->selection_snapshot_for_commands()) {
            return luaL_error(lua_state, "no selection");
        }
        std::string text = luaL_checkstring(lua_state, 1);
        bool changed = impl->current_state->replace_selection_for_commands(utf8_to_u32(text));
        lua_pushboolean(lua_state, changed ? 1 : 0);
        return 1;
    }

    static int lua_replace_buffer(lua_State *lua_state) {
        LuaRuntime::Impl *impl = from_upvalue(lua_state);
        if (!impl->with_current_state(lua_state)) {
            return lua_error(lua_state);
        }

        std::string text = luaL_checkstring(lua_state, 1);
        bool changed = impl->current_state->active_core().replace_range(
            full_buffer_range(impl->current_state->active_core()),
            utf8_to_u32(text));
        if (changed) {
            for (const EditorWindow &window : impl->current_state->windows.windows()) {
                if (window.buffer_id == impl->current_state->active_buffer().id) {
                    impl->current_state->sync_window_view_from_core(window.id);
                }
            }
        }
        lua_pushboolean(lua_state, changed ? 1 : 0);
        return 1;
    }

    static int lua_replace_range(lua_State *lua_state) {
        LuaRuntime::Impl *impl = from_upvalue(lua_state);
        if (!impl->with_current_state(lua_state)) {
            return lua_error(lua_state);
        }

        Range range = lua_check_range(lua_state, 1);
        std::string text = luaL_checkstring(lua_state, 2);
        bool changed = impl->current_state->active_core().replace_range(range, utf8_to_u32(text));
        lua_pushboolean(lua_state, changed ? 1 : 0);
        return 1;
    }

    static int lua_create_buffer(lua_State *lua_state) {
        LuaRuntime::Impl *impl = from_upvalue(lua_state);
        if (!impl->with_current_state(lua_state)) {
            return lua_error(lua_state);
        }

        std::string name = luaL_checkstring(lua_state, 1);
        std::string kind_name = lua_gettop(lua_state) >= 2 && !lua_isnil(lua_state, 2)
            ? luaL_checkstring(lua_state, 2)
            : "scratch";
        std::optional<EditorBufferKind> kind = parse_buffer_kind(kind_name);
        if (!kind) {
            return luaL_error(lua_state, "unsupported buffer kind: %s", kind_name.c_str());
        }

        EditorBuffer &buffer = impl->current_state->ensure_named_special_buffer(
            name,
            *kind,
            *kind == EditorBufferKind::Scratch);
        lua_pushinteger(lua_state, static_cast<lua_Integer>(buffer.id));
        return 1;
    }

    static int lua_append_buffer(lua_State *lua_state) {
        LuaRuntime::Impl *impl = from_upvalue(lua_state);
        if (!impl->with_current_state(lua_state)) {
            return lua_error(lua_state);
        }

        lua_Integer raw_buffer_id = luaL_checkinteger(lua_state, 1);
        if (raw_buffer_id <= 0) {
            return luaL_error(lua_state, "buffer id must be positive");
        }
        std::string text = luaL_checkstring(lua_state, 2);
        impl->current_state->append_to_buffer(static_cast<std::size_t>(raw_buffer_id), utf8_to_u32(text));
        return 0;
    }

    static int lua_clear_buffer(lua_State *lua_state) {
        LuaRuntime::Impl *impl = from_upvalue(lua_state);
        if (!impl->with_current_state(lua_state)) {
            return lua_error(lua_state);
        }

        lua_Integer raw_buffer_id = luaL_checkinteger(lua_state, 1);
        if (raw_buffer_id <= 0) {
            return luaL_error(lua_state, "buffer id must be positive");
        }
        impl->current_state->clear_buffer_contents(static_cast<std::size_t>(raw_buffer_id));
        return 0;
    }

    static int lua_show_buffer_in_panel(lua_State *lua_state) {
        LuaRuntime::Impl *impl = from_upvalue(lua_state);
        if (!impl->with_current_state(lua_state)) {
            return lua_error(lua_state);
        }

        lua_Integer raw_buffer_id = luaL_checkinteger(lua_state, 1);
        if (raw_buffer_id <= 0) {
            return luaL_error(lua_state, "buffer id must be positive");
        }
        bool focus_panel = lua_gettop(lua_state) >= 2 && lua_toboolean(lua_state, 2);
        impl->current_state->show_buffer_in_panel(static_cast<std::size_t>(raw_buffer_id), focus_panel);
        return 0;
    }

    static int lua_open_file(lua_State *lua_state) {
        LuaRuntime::Impl *impl = from_upvalue(lua_state);
        if (!impl->with_current_state(lua_state)) {
            return lua_error(lua_state);
        }

        std::string path = expand_user_path(luaL_checkstring(lua_state, 1));
        EditorBuffer *buffer = impl->current_state->session.open_file(path, true);
        if (buffer == nullptr) {
            return luaL_error(lua_state, "could not open file: %s", path.c_str());
        }
        impl->current_state->show_buffer_in_active_window(buffer->id);
        return 0;
    }

    static int lua_open_location(lua_State *lua_state) {
        LuaRuntime::Impl *impl = from_upvalue(lua_state);
        if (!impl->with_current_state(lua_state)) {
            return lua_error(lua_state);
        }

        std::string path = expand_user_path(luaL_checkstring(lua_state, 1));
        Position position = lua_check_position(lua_state, 2);

        EditorBuffer *existing = nullptr;
        for (EditorBuffer &buffer : impl->current_state->session.buffers()) {
            if (buffer.core.file_path() && *buffer.core.file_path() == path) {
                existing = &buffer;
                break;
            }
        }

        if (existing != nullptr) {
            if (std::optional<std::size_t> window_id = impl->current_state->windows.find_window_showing_buffer(existing->id)) {
                impl->current_state->focus_window(*window_id);
            } else {
                impl->current_state->show_buffer_in_active_window(existing->id);
            }
            impl->current_state->active_core().set_cursor(position);
            return 0;
        }

        EditorBuffer *buffer = impl->current_state->session.open_file(path, true);
        if (buffer == nullptr) {
            return luaL_error(lua_state, "could not open file: %s", path.c_str());
        }
        impl->current_state->show_buffer_in_active_window(buffer->id);
        impl->current_state->active_core().set_cursor(position);
        return 0;
    }

    static int lua_show_popup(lua_State *lua_state) {
        LuaRuntime::Impl *impl = from_upvalue(lua_state);
        if (!impl->with_current_state(lua_state)) {
            return lua_error(lua_state);
        }

        const char *title = luaL_checkstring(lua_state, 1);
        const char *text = luaL_checkstring(lua_state, 2);
        impl->current_state->show_popup(title, utf8_to_u32(text));
        return 0;
    }

    static int lua_list_themes(lua_State *lua_state) {
        LuaRuntime::Impl *impl = from_upvalue(lua_state);
        if (!impl->with_current_state(lua_state)) {
            return lua_error(lua_state);
        }

        std::filesystem::path theme_dir = theme_directory_for_config(impl->current_state->config);
        lua_createtable(lua_state, 0, 0);
        if (theme_dir.empty() || !std::filesystem::exists(theme_dir)) {
            return 1;
        }

        std::vector<std::string> theme_names;
        for (const auto &entry : std::filesystem::directory_iterator(theme_dir)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".json") {
                continue;
            }
            theme_names.push_back("themes/" + entry.path().filename().string());
        }
        std::sort(theme_names.begin(), theme_names.end());
        for (std::size_t index = 0; index < theme_names.size(); ++index) {
            lua_pushinteger(lua_state, static_cast<lua_Integer>(index + 1));
            lua_pushlstring(lua_state, theme_names[index].data(), theme_names[index].size());
            lua_settable(lua_state, -3);
        }
        return 1;
    }

    static int lua_set_config_value(lua_State *lua_state) {
        LuaRuntime::Impl *impl = from_upvalue(lua_state);
        if (!impl->with_current_state(lua_state)) {
            return lua_error(lua_state);
        }

        if (impl->current_state->config.source_path.empty()) {
            return luaL_error(lua_state, "config update requires a meditrc");
        }

        std::string key = luaL_checkstring(lua_state, 1);
        std::string value = luaL_checkstring(lua_state, 2);
        std::string error_message;
        if (!update_meditrc_setting(impl->current_state->config.source_path, key, value, error_message)) {
            return luaL_error(lua_state, "%s", error_message.c_str());
        }
        return 0;
    }

    static int lua_reload_config(lua_State *lua_state) {
        LuaRuntime::Impl *impl = from_upvalue(lua_state);
        if (!impl->with_current_state(lua_state)) {
            return lua_error(lua_state);
        }
        impl->current_state->request_config_reload();
        return 0;
    }

    static int lua_shell_quote(lua_State *lua_state) {
        std::string text = luaL_checkstring(lua_state, 1);
        std::string quoted = "'";
        for (char ch : text) {
            if (ch == '\'') {
                quoted += "'\\''";
            } else {
                quoted.push_back(ch);
            }
        }
        quoted.push_back('\'');
        lua_pushlstring(lua_state, quoted.data(), quoted.size());
        return 1;
    }

    static int lua_executable_exists(lua_State *lua_state) {
        const char *name = luaL_checkstring(lua_state, 1);
        lua_pushboolean(lua_state, executable_exists(name) ? 1 : 0);
        return 1;
    }

    static int lua_file_exists(lua_State *lua_state) {
        std::filesystem::path path = luaL_checkstring(lua_state, 1);
        lua_pushboolean(lua_state, std::filesystem::exists(path) && std::filesystem::is_regular_file(path) ? 1 : 0);
        return 1;
    }

    static int lua_theme_color_supported(lua_State *lua_state) {
        std::string value = luaL_checkstring(lua_state, 1);
        lua_pushboolean(lua_state, try_parse_theme_color(value).has_value() ? 1 : 0);
        return 1;
    }

    static int lua_run_picker(lua_State *lua_state) {
        LuaRuntime::Impl *impl = from_upvalue(lua_state);
        if (!impl->with_current_state(lua_state)) {
            return lua_error(lua_state);
        }

        std::string command = luaL_checkstring(lua_state, 1);
        std::string error_message;
        std::optional<std::string> selection = impl->current_state->run_picker_command(command, error_message);
        if (!selection) {
            lua_pushnil(lua_state);
            lua_pushlstring(lua_state, error_message.data(), error_message.size());
            return 2;
        }
        lua_pushlstring(lua_state, selection->data(), selection->size());
        return 1;
    }

    static int lua_run_filter(lua_State *lua_state) {
        LuaRuntime::Impl *impl = from_upvalue(lua_state);
        if (!impl->with_current_state(lua_state)) {
            return lua_error(lua_state);
        }

        std::string command = luaL_checkstring(lua_state, 1);
        std::string input = luaL_checkstring(lua_state, 2);
        std::u32string output_text;
        std::string error_message;
        if (!impl->current_state->run_filter_command(command, utf8_to_u32(input), output_text, error_message)) {
            lua_pushnil(lua_state);
            lua_pushlstring(lua_state, error_message.data(), error_message.size());
            return 2;
        }

        std::string output = u32_to_utf8(output_text);
        lua_pushlstring(lua_state, output.data(), output.size());
        return 1;
    }

    static int lua_resolve_ai_command(lua_State *lua_state) {
        LuaRuntime::Impl *impl = from_upvalue(lua_state);
        if (!impl->with_current_state(lua_state)) {
            return lua_error(lua_state);
        }

        std::optional<std::string> command = resolve_ai_command(impl->current_state->config);
        if (!command) {
            lua_pushnil(lua_state);
            return 1;
        }
        lua_pushlstring(lua_state, command->data(), command->size());
        return 1;
    }

    static int lua_resolve_ai_provider(lua_State *lua_state) {
        LuaRuntime::Impl *impl = from_upvalue(lua_state);
        if (!impl->with_current_state(lua_state)) {
            return lua_error(lua_state);
        }

        std::string provider = resolve_ai_provider(impl->current_state->config);
        lua_pushlstring(lua_state, provider.data(), provider.size());
        return 1;
    }

    static int lua_resolve_ai_model(lua_State *lua_state) {
        LuaRuntime::Impl *impl = from_upvalue(lua_state);
        if (!impl->with_current_state(lua_state)) {
            return lua_error(lua_state);
        }

        std::string provider;
        if (lua_gettop(lua_state) >= 1 && !lua_isnil(lua_state, 1)) {
            provider = luaL_checkstring(lua_state, 1);
        } else {
            provider = resolve_ai_provider(impl->current_state->config);
        }
        std::string model = resolve_ai_model(impl->current_state->config, provider);
        lua_pushlstring(lua_state, model.data(), model.size());
        return 1;
    }

    static int lua_register_command(lua_State *lua_state) {
        LuaRuntime::Impl *impl = from_upvalue(lua_state);
        const char *name = luaL_checkstring(lua_state, 1);
        luaL_checktype(lua_state, 2, LUA_TFUNCTION);

        LuaCommandInfo info;
        info.name = name;
        info.completion_text = name;
        if (lua_gettop(lua_state) >= 3 && !lua_isnil(lua_state, 3)) {
            luaL_checktype(lua_state, 3, LUA_TTABLE);

            lua_getfield(lua_state, 3, "detail");
            if (!lua_isnil(lua_state, -1)) {
                info.detail = luaL_checkstring(lua_state, -1);
            }
            lua_pop(lua_state, 1);

            lua_getfield(lua_state, 3, "completion_text");
            if (!lua_isnil(lua_state, -1)) {
                info.completion_text = luaL_checkstring(lua_state, -1);
            }
            lua_pop(lua_state, 1);

            lua_getfield(lua_state, 3, "aliases");
            if (!lua_isnil(lua_state, -1)) {
                luaL_checktype(lua_state, -1, LUA_TTABLE);
                lua_pushnil(lua_state);
                while (lua_next(lua_state, -2) != 0) {
                    const char *alias_text = luaL_checkstring(lua_state, -1);
                    std::string alias = alias_text;
                    if (!alias.empty() && alias != info.name &&
                        std::find(info.aliases.begin(), info.aliases.end(), alias) == info.aliases.end()) {
                        info.aliases.push_back(std::move(alias));
                    }
                    lua_pop(lua_state, 1);
                }
            }
            lua_pop(lua_state, 1);
        }

        lua_pushvalue(lua_state, 2);
        int ref = luaL_ref(lua_state, LUA_REGISTRYINDEX);
        auto existing = impl->commands.find(info.name);
        if (existing != impl->commands.end()) {
            impl->unregister_command_aliases(existing->first);
            luaL_unref(lua_state, LUA_REGISTRYINDEX, existing->second.ref);
        }
        impl->commands[info.name] = {ref, std::move(info)};
        for (const std::string &alias : impl->commands[info.name].info.aliases) {
            impl->command_aliases[alias] = impl->commands[info.name].info.name;
        }
        return 0;
    }

    static int lua_register_health_check(lua_State *lua_state) {
        LuaRuntime::Impl *impl = from_upvalue(lua_state);
        const char *name = luaL_checkstring(lua_state, 1);
        luaL_checktype(lua_state, 2, LUA_TFUNCTION);

        lua_pushvalue(lua_state, 2);
        int ref = luaL_ref(lua_state, LUA_REGISTRYINDEX);
        auto existing = impl->health_check_refs.find(name);
        if (existing != impl->health_check_refs.end()) {
            luaL_unref(lua_state, LUA_REGISTRYINDEX, existing->second);
        }
        impl->health_check_refs[name] = ref;
        return 0;
    }

    static int lua_job_start(lua_State *lua_state) {
        LuaRuntime::Impl *impl = from_upvalue(lua_state);
        if (!impl->with_current_state(lua_state)) {
            return lua_error(lua_state);
        }

        luaL_checktype(lua_state, 1, LUA_TTABLE);
        lua_getfield(lua_state, 1, "command");
        const char *command_text = luaL_checkstring(lua_state, -1);
        std::string command = command_text;
        lua_pop(lua_state, 1);

        std::optional<std::size_t> buffer_id;
        lua_getfield(lua_state, 1, "buffer_id");
        if (!lua_isnil(lua_state, -1)) {
            lua_Integer raw_buffer_id = luaL_checkinteger(lua_state, -1);
            if (raw_buffer_id <= 0) {
                lua_pop(lua_state, 1);
                return luaL_error(lua_state, "buffer_id must be positive");
            }
            buffer_id = static_cast<std::size_t>(raw_buffer_id);
            if (impl->current_state->session.find_buffer_by_id(*buffer_id) == nullptr) {
                lua_pop(lua_state, 1);
                return luaL_error(lua_state, "buffer_id not found");
            }
        }
        lua_pop(lua_state, 1);

        int on_exit_ref = LUA_NOREF;
        lua_getfield(lua_state, 1, "on_exit");
        if (!lua_isnil(lua_state, -1)) {
            luaL_checktype(lua_state, -1, LUA_TFUNCTION);
            on_exit_ref = luaL_ref(lua_state, LUA_REGISTRYINDEX);
        } else {
            lua_pop(lua_state, 1);
        }

        const std::size_t job_id = impl->next_async_job_id++;
        impl->async_jobs[job_id] = AsyncJobInfo{
            .id = job_id,
            .command = command,
            .buffer_id = buffer_id,
            .on_exit_ref = on_exit_ref,
            .running = true,
            .exit_code = -1,
        };

        std::thread(run_async_shell_command, impl->async_job_queue, job_id, command).detach();
        lua_pushinteger(lua_state, static_cast<lua_Integer>(job_id));
        return 1;
    }

    static int lua_job_status(lua_State *lua_state) {
        LuaRuntime::Impl *impl = from_upvalue(lua_state);
        lua_Integer raw_job_id = luaL_checkinteger(lua_state, 1);
        if (raw_job_id <= 0) {
            return luaL_error(lua_state, "job id must be positive");
        }

        auto found = impl->async_jobs.find(static_cast<std::size_t>(raw_job_id));
        if (found == impl->async_jobs.end()) {
            lua_pushnil(lua_state);
            return 1;
        }

        const AsyncJobInfo &job = found->second;
        lua_createtable(lua_state, 0, 4);
        lua_pushboolean(lua_state, job.running ? 1 : 0);
        lua_setfield(lua_state, -2, "running");
        lua_pushinteger(lua_state, static_cast<lua_Integer>(job.exit_code));
        lua_setfield(lua_state, -2, "exit_code");
        lua_pushlstring(lua_state, job.command.data(), job.command.size());
        lua_setfield(lua_state, -2, "command");
        if (job.buffer_id) {
            lua_pushinteger(lua_state, static_cast<lua_Integer>(*job.buffer_id));
        } else {
            lua_pushnil(lua_state);
        }
        lua_setfield(lua_state, -2, "buffer_id");
        return 1;
    }

    static int lua_process_start(lua_State *lua_state) {
        LuaRuntime::Impl *impl = from_upvalue(lua_state);
        if (!impl->with_current_state(lua_state)) {
            return lua_error(lua_state);
        }

#if !defined(__unix__) && !defined(__APPLE__)
        return luaL_error(lua_state, "persistent processes are unsupported on this platform");
#else
        luaL_checktype(lua_state, 1, LUA_TTABLE);
        lua_getfield(lua_state, 1, "command");
        std::string command = luaL_checkstring(lua_state, -1);
        lua_pop(lua_state, 1);

        std::optional<std::size_t> buffer_id;
        lua_getfield(lua_state, 1, "buffer_id");
        if (!lua_isnil(lua_state, -1)) {
            lua_Integer raw_buffer_id = luaL_checkinteger(lua_state, -1);
            if (raw_buffer_id <= 0) {
                lua_pop(lua_state, 1);
                return luaL_error(lua_state, "buffer_id must be positive");
            }
            buffer_id = static_cast<std::size_t>(raw_buffer_id);
            if (impl->current_state->session.find_buffer_by_id(*buffer_id) == nullptr) {
                lua_pop(lua_state, 1);
                return luaL_error(lua_state, "buffer_id not found");
            }
        }
        lua_pop(lua_state, 1);

        auto load_callback_ref = [&](const char *field_name) {
            int ref = LUA_NOREF;
            lua_getfield(lua_state, 1, field_name);
            if (!lua_isnil(lua_state, -1)) {
                luaL_checktype(lua_state, -1, LUA_TFUNCTION);
                ref = luaL_ref(lua_state, LUA_REGISTRYINDEX);
            } else {
                lua_pop(lua_state, 1);
            }
            return ref;
        };

        int on_stdout_ref = load_callback_ref("on_stdout");
        int on_stderr_ref = load_callback_ref("on_stderr");
        int on_exit_ref = load_callback_ref("on_exit");

        const std::size_t process_id = impl->next_process_id++;
        ProcessInfo process;
        process.id = process_id;
        process.command = command;
        process.buffer_id = buffer_id;
        process.on_stdout_ref = on_stdout_ref;
        process.on_stderr_ref = on_stderr_ref;
        process.on_exit_ref = on_exit_ref;
        process.running = true;
        process.exit_code = -1;

        std::string error_message;
        if (!spawn_shell_process(
                impl->process_event_queue,
                process_id,
                command,
                process.pid,
                process.stdin_fd,
                error_message)) {
            impl->release_process_refs(process);
            return luaL_error(lua_state, "%s", error_message.c_str());
        }

        impl->processes[process_id] = std::move(process);
        lua_pushinteger(lua_state, static_cast<lua_Integer>(process_id));
        return 1;
#endif
    }

    static int lua_process_send(lua_State *lua_state) {
        LuaRuntime::Impl *impl = from_upvalue(lua_state);
#if !defined(__unix__) && !defined(__APPLE__)
        (void)impl;
        return luaL_error(lua_state, "persistent processes are unsupported on this platform");
#else
        lua_Integer raw_process_id = luaL_checkinteger(lua_state, 1);
        if (raw_process_id <= 0) {
            return luaL_error(lua_state, "process id must be positive");
        }
        std::string text = luaL_checkstring(lua_state, 2);
        auto found = impl->processes.find(static_cast<std::size_t>(raw_process_id));
        if (found == impl->processes.end()) {
            return luaL_error(lua_state, "process not found");
        }

        ProcessInfo &process = found->second;
        if (!process.running || process.stdin_fd < 0) {
            lua_pushboolean(lua_state, 0);
            return 1;
        }

        const char *data = text.data();
        std::size_t remaining = text.size();
        while (remaining > 0) {
            ssize_t written = write(process.stdin_fd, data, remaining);
            if (written > 0) {
                data += written;
                remaining -= static_cast<std::size_t>(written);
                continue;
            }
            if (written < 0 && errno == EINTR) {
                continue;
            }
            lua_pushboolean(lua_state, 0);
            return 1;
        }
        lua_pushboolean(lua_state, 1);
        return 1;
#endif
    }

    static int lua_process_close_stdin(lua_State *lua_state) {
        LuaRuntime::Impl *impl = from_upvalue(lua_state);
        lua_Integer raw_process_id = luaL_checkinteger(lua_state, 1);
        if (raw_process_id <= 0) {
            return luaL_error(lua_state, "process id must be positive");
        }
        auto found = impl->processes.find(static_cast<std::size_t>(raw_process_id));
        if (found == impl->processes.end()) {
            return luaL_error(lua_state, "process not found");
        }
        impl->close_process_stdin(found->second);
        return 0;
    }

    static int lua_process_status(lua_State *lua_state) {
        LuaRuntime::Impl *impl = from_upvalue(lua_state);
        lua_Integer raw_process_id = luaL_checkinteger(lua_state, 1);
        if (raw_process_id <= 0) {
            return luaL_error(lua_state, "process id must be positive");
        }

        auto found = impl->processes.find(static_cast<std::size_t>(raw_process_id));
        if (found == impl->processes.end()) {
            lua_pushnil(lua_state);
            return 1;
        }

        const ProcessInfo &process = found->second;
        lua_createtable(lua_state, 0, 4);
        lua_pushboolean(lua_state, process.running ? 1 : 0);
        lua_setfield(lua_state, -2, "running");
        lua_pushinteger(lua_state, static_cast<lua_Integer>(process.exit_code));
        lua_setfield(lua_state, -2, "exit_code");
        lua_pushlstring(lua_state, process.command.data(), process.command.size());
        lua_setfield(lua_state, -2, "command");
        if (process.buffer_id) {
            lua_pushinteger(lua_state, static_cast<lua_Integer>(*process.buffer_id));
        } else {
            lua_pushnil(lua_state);
        }
        lua_setfield(lua_state, -2, "buffer_id");
        return 1;
    }

    static int lua_process_stop(lua_State *lua_state) {
        LuaRuntime::Impl *impl = from_upvalue(lua_state);
        lua_Integer raw_process_id = luaL_checkinteger(lua_state, 1);
        if (raw_process_id <= 0) {
            return luaL_error(lua_state, "process id must be positive");
        }

        auto found = impl->processes.find(static_cast<std::size_t>(raw_process_id));
        if (found == impl->processes.end()) {
            return luaL_error(lua_state, "process not found");
        }
        impl->stop_process(found->second);
        return 0;
    }

    static int lua_on(lua_State *lua_state) {
        LuaRuntime::Impl *impl = from_upvalue(lua_state);
        const char *name = luaL_checkstring(lua_state, 1);
        luaL_checktype(lua_state, 2, LUA_TFUNCTION);

        std::string event = name;
        if (event != "document_opened" && event != "document_changed" && event != "document_saved" &&
            event != "document_closed" && event != "cursor_moved" && event != "diagnostics_changed" &&
            event != "annotations_changed") {
            return luaL_error(lua_state, "unsupported event: %s", name);
        }

        lua_pushvalue(lua_state, 2);
        int ref = luaL_ref(lua_state, LUA_REGISTRYINDEX);
        impl->event_refs[event].push_back(ref);
        return 0;
    }

    void clear_refs() {
        if (lua == nullptr) {
            commands.clear();
            command_aliases.clear();
            event_refs.clear();
            health_check_refs.clear();
            async_jobs.clear();
            processes.clear();
            return;
        }
        for (auto &[_, command] : commands) {
            luaL_unref(lua, LUA_REGISTRYINDEX, command.ref);
        }
        commands.clear();
        command_aliases.clear();
        for (auto &[_, ref] : health_check_refs) {
            luaL_unref(lua, LUA_REGISTRYINDEX, ref);
        }
        health_check_refs.clear();
        for (auto &[_, refs] : event_refs) {
            for (int ref : refs) {
                luaL_unref(lua, LUA_REGISTRYINDEX, ref);
            }
        }
        event_refs.clear();
        for (auto &[_, job] : async_jobs) {
            if (job.on_exit_ref != LUA_NOREF) {
                luaL_unref(lua, LUA_REGISTRYINDEX, job.on_exit_ref);
            }
        }
        async_jobs.clear();
        for (auto &[_, process] : processes) {
            stop_process(process);
            release_process_refs(process);
        }
        processes.clear();
    }

    void register_api() {
        lua_newtable(lua);

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_status, 1);
        lua_setfield(lua, -2, "status");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_set_status, 1);
        lua_setfield(lua, -2, "set_status");

        lua_pushcfunction(lua, &LuaRuntime::Impl::lua_current_working_directory);
        lua_setfield(lua, -2, "current_working_directory");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_current_file_path, 1);
        lua_setfield(lua, -2, "current_file_path");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_workspace_root, 1);
        lua_setfield(lua, -2, "workspace_root");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_token_under_cursor, 1);
        lua_setfield(lua, -2, "token_under_cursor");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_get_cursor, 1);
        lua_setfield(lua, -2, "get_cursor");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_get_selection, 1);
        lua_setfield(lua, -2, "get_selection");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_get_selection_text, 1);
        lua_setfield(lua, -2, "get_selection_text");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_get_buffer_text, 1);
        lua_setfield(lua, -2, "get_buffer_text");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_get_line_text, 1);
        lua_setfield(lua, -2, "get_line_text");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_set_line_annotations, 1);
        lua_setfield(lua, -2, "set_line_annotations");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_clear_line_annotations, 1);
        lua_setfield(lua, -2, "clear_line_annotations");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_get_text, 1);
        lua_setfield(lua, -2, "get_text");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_set_cursor, 1);
        lua_setfield(lua, -2, "set_cursor");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_replace_selection, 1);
        lua_setfield(lua, -2, "replace_selection");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_replace_buffer, 1);
        lua_setfield(lua, -2, "replace_buffer");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_replace_range, 1);
        lua_setfield(lua, -2, "replace_range");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_create_buffer, 1);
        lua_setfield(lua, -2, "create_buffer");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_append_buffer, 1);
        lua_setfield(lua, -2, "append_buffer");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_clear_buffer, 1);
        lua_setfield(lua, -2, "clear_buffer");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_show_buffer_in_panel, 1);
        lua_setfield(lua, -2, "show_buffer_in_panel");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_open_file, 1);
        lua_setfield(lua, -2, "open_file");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_open_location, 1);
        lua_setfield(lua, -2, "open_location");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_list_themes, 1);
        lua_setfield(lua, -2, "list_themes");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_set_config_value, 1);
        lua_setfield(lua, -2, "set_config_value");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_reload_config, 1);
        lua_setfield(lua, -2, "reload_config");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_show_popup, 1);
        lua_setfield(lua, -2, "show_popup");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_executable_exists, 1);
        lua_setfield(lua, -2, "executable_exists");

        lua_pushcfunction(lua, &LuaRuntime::Impl::lua_file_exists);
        lua_setfield(lua, -2, "file_exists");

        lua_pushcfunction(lua, &LuaRuntime::Impl::lua_theme_color_supported);
        lua_setfield(lua, -2, "theme_color_supported");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_run_picker, 1);
        lua_setfield(lua, -2, "run_picker");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_run_filter, 1);
        lua_setfield(lua, -2, "run_filter");

        lua_pushcfunction(lua, &LuaRuntime::Impl::lua_shell_quote);
        lua_setfield(lua, -2, "shell_quote");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_resolve_ai_command, 1);
        lua_setfield(lua, -2, "resolve_ai_command");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_resolve_ai_provider, 1);
        lua_setfield(lua, -2, "resolve_ai_provider");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_resolve_ai_model, 1);
        lua_setfield(lua, -2, "resolve_ai_model");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_register_command, 1);
        lua_setfield(lua, -2, "register_command");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_register_health_check, 1);
        lua_setfield(lua, -2, "register_health_check");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_job_start, 1);
        lua_setfield(lua, -2, "job_start");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_job_status, 1);
        lua_setfield(lua, -2, "job_status");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_process_start, 1);
        lua_setfield(lua, -2, "process_start");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_process_send, 1);
        lua_setfield(lua, -2, "process_send");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_process_close_stdin, 1);
        lua_setfield(lua, -2, "process_close_stdin");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_process_status, 1);
        lua_setfield(lua, -2, "process_status");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_process_stop, 1);
        lua_setfield(lua, -2, "process_stop");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_on, 1);
        lua_setfield(lua, -2, "on");

        lua_setglobal(lua, "medit");
    }

    bool protected_call(EditorState &state, int argument_count, int result_count, std::string &error_message) {
        ScopedLuaEditorState scoped(current_state, state);
        if (lua_pcall(lua, argument_count, result_count, 0) == LUA_OK) {
            return true;
        }
        const char *message = lua_tostring(lua, -1);
        error_message = message != nullptr ? message : "unknown Lua error";
        lua_pop(lua, 1);
        return false;
    }
#endif
};

LuaRuntime::LuaRuntime() : impl_(std::make_unique<Impl>()) {}

LuaRuntime::~LuaRuntime() {
    shutdown();
}

bool LuaRuntime::available() const {
#if defined(MEDIT_HAS_LUA) && MEDIT_HAS_LUA
    return true;
#else
    return false;
#endif
}

bool LuaRuntime::enabled() const {
    return impl_->initialized;
}

bool LuaRuntime::initialize(
    EditorState &state,
    const std::optional<std::filesystem::path> &script_path,
    std::string &error_message) {
    shutdown();
    if (!script_path) {
        return true;
    }

#if defined(MEDIT_HAS_LUA) && MEDIT_HAS_LUA
    impl_->lua = luaL_newstate();
    if (impl_->lua == nullptr) {
        error_message = "could not create Lua state";
        return false;
    }

    luaL_openlibs(impl_->lua);
    impl_->register_api();

    ScopedLuaEditorState scoped(impl_->current_state, state);
    if (luaL_loadfile(impl_->lua, script_path->c_str()) != LUA_OK) {
        const char *message = lua_tostring(impl_->lua, -1);
        error_message = message != nullptr ? message : "could not load Lua script";
        lua_pop(impl_->lua, 1);
        shutdown();
        return false;
    }

    if (lua_pcall(impl_->lua, 0, 0, 0) != LUA_OK) {
        const char *message = lua_tostring(impl_->lua, -1);
        error_message = message != nullptr ? message : "could not run Lua script";
        lua_pop(impl_->lua, 1);
        shutdown();
        return false;
    }

    impl_->initialized = true;
    return true;
#else
    (void)state;
    error_message = "Lua support not built in";
    return false;
#endif
}

void LuaRuntime::shutdown() {
#if defined(MEDIT_HAS_LUA) && MEDIT_HAS_LUA
    impl_->initialized = false;
    if (impl_->lua != nullptr) {
        impl_->clear_refs();
        lua_close(impl_->lua);
        impl_->lua = nullptr;
    }
    impl_->current_state = nullptr;
#else
    impl_->initialized = false;
#endif
}

bool LuaRuntime::execute_command(
    EditorState &state,
    const std::string &name,
    const std::string &argument,
    std::string &error_message) {
#if defined(MEDIT_HAS_LUA) && MEDIT_HAS_LUA
    if (!impl_->initialized || impl_->lua == nullptr) {
        error_message = "Lua runtime not initialized";
        return false;
    }

    std::optional<std::string> resolved_name = impl_->resolve_command_name(name);
    if (!resolved_name) {
        error_message = "No such Lua command: " + name;
        return false;
    }

    lua_rawgeti(impl_->lua, LUA_REGISTRYINDEX, impl_->commands[*resolved_name].ref);
    lua_pushlstring(impl_->lua, argument.data(), argument.size());
    return impl_->protected_call(state, 1, 0, error_message);
#else
    (void)state;
    (void)name;
    (void)argument;
    error_message = "Lua support not built in";
    return false;
#endif
}

void LuaRuntime::detach_async_buffer(std::size_t buffer_id) {
#if defined(MEDIT_HAS_LUA) && MEDIT_HAS_LUA
    for (auto &[_, job] : impl_->async_jobs) {
        if (job.buffer_id == buffer_id) {
            job.buffer_id.reset();
        }
    }
    for (auto &[_, process] : impl_->processes) {
        if (process.buffer_id == buffer_id) {
            process.buffer_id.reset();
        }
    }
#else
    (void)buffer_id;
#endif
}

void LuaRuntime::poll_async(EditorState &state) {
#if defined(MEDIT_HAS_LUA) && MEDIT_HAS_LUA
    if (!impl_->initialized || impl_->lua == nullptr) {
        return;
    }

    std::deque<AsyncJobEvent> events;
    {
        std::lock_guard<std::mutex> lock(impl_->async_job_queue->mutex);
        events.swap(impl_->async_job_queue->events);
    }
    std::deque<ProcessEvent> process_events;
    {
        std::lock_guard<std::mutex> lock(impl_->process_event_queue->mutex);
        process_events.swap(impl_->process_event_queue->events);
    }

    for (const AsyncJobEvent &event : events) {
        auto found = impl_->async_jobs.find(event.job_id);
        if (found == impl_->async_jobs.end()) {
            continue;
        }
        LuaRuntime::Impl::AsyncJobInfo &job = found->second;

        if (event.type == AsyncJobEvent::Type::Output) {
            if (job.buffer_id && !event.text.empty()) {
                state.append_to_buffer(*job.buffer_id, utf8_to_u32(event.text));
            }
            continue;
        }

        job.running = false;
        job.exit_code = event.exit_code;
        if (job.on_exit_ref != LUA_NOREF) {
            lua_rawgeti(impl_->lua, LUA_REGISTRYINDEX, job.on_exit_ref);
            lua_pushinteger(impl_->lua, static_cast<lua_Integer>(job.id));
            lua_pushinteger(impl_->lua, static_cast<lua_Integer>(job.exit_code));
            std::string error_message;
            if (!impl_->protected_call(state, 2, 0, error_message)) {
                log_debug("lua async on_exit failed job=" + std::to_string(job.id) + " error=" + error_message);
                state.set_status("Lua async callback failed: " + error_message);
            }
            luaL_unref(impl_->lua, LUA_REGISTRYINDEX, job.on_exit_ref);
            job.on_exit_ref = LUA_NOREF;
        }
    }

    for (const ProcessEvent &event : process_events) {
        auto found = impl_->processes.find(event.process_id);
        if (found == impl_->processes.end()) {
            continue;
        }
        LuaRuntime::Impl::ProcessInfo &process = found->second;

        if (event.type == ProcessEvent::Type::Stdout || event.type == ProcessEvent::Type::Stderr) {
            if (process.buffer_id && !event.text.empty()) {
                state.append_to_buffer(*process.buffer_id, utf8_to_u32(event.text));
            }
            int callback_ref =
                event.type == ProcessEvent::Type::Stdout ? process.on_stdout_ref : process.on_stderr_ref;
            if (callback_ref != LUA_NOREF) {
                lua_rawgeti(impl_->lua, LUA_REGISTRYINDEX, callback_ref);
                lua_pushinteger(impl_->lua, static_cast<lua_Integer>(process.id));
                lua_pushlstring(impl_->lua, event.text.data(), event.text.size());
                std::string error_message;
                if (!impl_->protected_call(state, 2, 0, error_message)) {
                    log_debug(
                        "lua process callback failed id=" + std::to_string(process.id) + " error=" + error_message);
                    state.set_status("Lua process callback failed: " + error_message);
                }
            }
            continue;
        }

        process.running = false;
        process.exit_code = event.exit_code;
        impl_->close_process_stdin(process);
        if (process.on_exit_ref != LUA_NOREF) {
            lua_rawgeti(impl_->lua, LUA_REGISTRYINDEX, process.on_exit_ref);
            lua_pushinteger(impl_->lua, static_cast<lua_Integer>(process.id));
            lua_pushinteger(impl_->lua, static_cast<lua_Integer>(process.exit_code));
            std::string error_message;
            if (!impl_->protected_call(state, 2, 0, error_message)) {
                log_debug("lua process on_exit failed id=" + std::to_string(process.id) + " error=" + error_message);
                state.set_status("Lua process callback failed: " + error_message);
            }
        }
        impl_->release_process_refs(process);
    }
#else
    (void)state;
#endif
}

std::optional<int> LuaRuntime::idle_wait_timeout_ms() const {
#if defined(MEDIT_HAS_LUA) && MEDIT_HAS_LUA
    for (const auto &[_, job] : impl_->async_jobs) {
        if (job.running) {
            return 50;
        }
    }
    for (const auto &[_, process] : impl_->processes) {
        if (process.running) {
            return 50;
        }
    }
#endif
    return std::nullopt;
}

void LuaRuntime::dispatch_editor_event(EditorState &state, const EditorEvent &event) {
#if defined(MEDIT_HAS_LUA) && MEDIT_HAS_LUA
    if (!impl_->initialized || impl_->lua == nullptr) {
        return;
    }

    auto found = impl_->event_refs.find(event_name(event.type));
    if (found == impl_->event_refs.end()) {
        return;
    }

    std::optional<std::size_t> buffer_id;
    if (!event.document_uri.empty()) {
        if (EditorBuffer *buffer = state.session.find_buffer_by_uri(event.document_uri)) {
            buffer_id = buffer->id;
        }
    }

    for (int ref : found->second) {
        lua_rawgeti(impl_->lua, LUA_REGISTRYINDEX, ref);
        push_editor_event(impl_->lua, event, buffer_id);
        std::string error_message;
        if (!impl_->protected_call(state, 1, 0, error_message)) {
            log_debug("lua hook error event=" + std::string(event_name(event.type)) + " error=" + error_message);
            state.set_status("Lua hook failed: " + error_message);
        }
    }
#else
    (void)state;
    (void)event;
#endif
}

std::vector<std::string> LuaRuntime::registered_commands() const {
    std::vector<std::string> names;
#if defined(MEDIT_HAS_LUA) && MEDIT_HAS_LUA
    names.reserve(impl_->commands.size());
    for (const auto &[name, _] : impl_->commands) {
        names.push_back(name);
    }
#endif
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<LuaCommandInfo> LuaRuntime::registered_command_infos() const {
    std::vector<LuaCommandInfo> infos;
#if defined(MEDIT_HAS_LUA) && MEDIT_HAS_LUA
    infos.reserve(impl_->commands.size());
    for (const auto &[_, command] : impl_->commands) {
        infos.push_back(command.info);
    }
#endif
    std::sort(infos.begin(), infos.end(), [](const LuaCommandInfo &left, const LuaCommandInfo &right) {
        return left.name < right.name;
    });
    return infos;
}

std::vector<std::pair<std::string, std::string>> LuaRuntime::run_health_checks(EditorState &state) const {
    std::vector<std::pair<std::string, std::string>> results;
#if defined(MEDIT_HAS_LUA) && MEDIT_HAS_LUA
    if (!impl_->initialized || impl_->lua == nullptr) {
        return results;
    }

    results.reserve(impl_->health_check_refs.size());
    for (const auto &[name, ref] : impl_->health_check_refs) {
        lua_rawgeti(impl_->lua, LUA_REGISTRYINDEX, ref);
        std::string error_message;
        if (!impl_->protected_call(state, 0, 1, error_message)) {
            results.push_back({name, "error: " + error_message});
            continue;
        }

        std::string status;
        if (lua_isnil(impl_->lua, -1)) {
            status = "(nil)";
        } else {
            const char *text = lua_tostring(impl_->lua, -1);
            status = text != nullptr ? text : "(non-string result)";
        }
        lua_pop(impl_->lua, 1);
        results.push_back({name, status});
    }
    std::sort(results.begin(), results.end());
#else
    (void)state;
#endif
    return results;
}
