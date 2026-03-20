#include "lua_runtime.hpp"

#include "editor_internal.hpp"
#include "logger.hpp"
#include "process_utils.hpp"
#include "string_utils.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if defined(MEDIT_HAS_LUA) && MEDIT_HAS_LUA
extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}
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

void push_range(lua_State *lua, const Range &range) {
    lua_createtable(lua, 0, 2);
    push_position(lua, range.start);
    lua_setfield(lua, -2, "start");
    push_position(lua, range.end);
    lua_setfield(lua, -2, "end");
}

void push_editor_event(lua_State *lua, const EditorEvent &event) {
    lua_createtable(lua, 0, 6);
    lua_pushstring(lua, event_name(event.type));
    lua_setfield(lua, -2, "type");
    lua_pushlstring(lua, event.document_uri.data(), event.document_uri.size());
    lua_setfield(lua, -2, "document_uri");
    lua_pushinteger(lua, static_cast<lua_Integer>(event.document_version));
    lua_setfield(lua, -2, "document_version");
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

#endif

}  // namespace

struct LuaRuntime::Impl {
    bool initialized = false;

#if defined(MEDIT_HAS_LUA) && MEDIT_HAS_LUA
    lua_State *lua = nullptr;
    EditorState *current_state = nullptr;
    std::map<std::string, int> command_refs;
    std::map<std::string, std::vector<int>> event_refs;
    std::map<std::string, int> health_check_refs;

    static LuaRuntime::Impl *from_upvalue(lua_State *lua_state) {
        void *raw = lua_touserdata(lua_state, lua_upvalueindex(1));
        return static_cast<LuaRuntime::Impl *>(raw);
    }

    bool with_current_state(lua_State *lua_state) const {
        if (current_state != nullptr) {
            return true;
        }
        lua_pushstring(lua_state, "medit API unavailable outside editor callback");
        return false;
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

    static int lua_get_cursor(lua_State *lua_state) {
        LuaRuntime::Impl *impl = from_upvalue(lua_state);
        if (!impl->with_current_state(lua_state)) {
            return lua_error(lua_state);
        }

        push_position(lua_state, impl->current_state->displayed_cursor(impl->current_state->windows.active_window_id()));
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

    static int lua_register_command(lua_State *lua_state) {
        LuaRuntime::Impl *impl = from_upvalue(lua_state);
        const char *name = luaL_checkstring(lua_state, 1);
        luaL_checktype(lua_state, 2, LUA_TFUNCTION);

        lua_pushvalue(lua_state, 2);
        int ref = luaL_ref(lua_state, LUA_REGISTRYINDEX);
        auto existing = impl->command_refs.find(name);
        if (existing != impl->command_refs.end()) {
            luaL_unref(lua_state, LUA_REGISTRYINDEX, existing->second);
        }
        impl->command_refs[name] = ref;
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
            command_refs.clear();
            event_refs.clear();
            return;
        }
        for (auto &[_, ref] : command_refs) {
            luaL_unref(lua, LUA_REGISTRYINDEX, ref);
        }
        command_refs.clear();
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
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_get_cursor, 1);
        lua_setfield(lua, -2, "get_cursor");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_set_cursor, 1);
        lua_setfield(lua, -2, "set_cursor");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_open_file, 1);
        lua_setfield(lua, -2, "open_file");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_open_location, 1);
        lua_setfield(lua, -2, "open_location");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_show_popup, 1);
        lua_setfield(lua, -2, "show_popup");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_executable_exists, 1);
        lua_setfield(lua, -2, "executable_exists");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_run_picker, 1);
        lua_setfield(lua, -2, "run_picker");

        lua_pushcfunction(lua, &LuaRuntime::Impl::lua_shell_quote);
        lua_setfield(lua, -2, "shell_quote");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_register_command, 1);
        lua_setfield(lua, -2, "register_command");

        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, &LuaRuntime::Impl::lua_register_health_check, 1);
        lua_setfield(lua, -2, "register_health_check");

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

    auto found = impl_->command_refs.find(name);
    if (found == impl_->command_refs.end()) {
        error_message = "No such Lua command: " + name;
        return false;
    }

    lua_rawgeti(impl_->lua, LUA_REGISTRYINDEX, found->second);
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

void LuaRuntime::dispatch_editor_event(EditorState &state, const EditorEvent &event) {
#if defined(MEDIT_HAS_LUA) && MEDIT_HAS_LUA
    if (!impl_->initialized || impl_->lua == nullptr) {
        return;
    }

    auto found = impl_->event_refs.find(event_name(event.type));
    if (found == impl_->event_refs.end()) {
        return;
    }

    for (int ref : found->second) {
        lua_rawgeti(impl_->lua, LUA_REGISTRYINDEX, ref);
        push_editor_event(impl_->lua, event);
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
    names.reserve(impl_->command_refs.size());
    for (const auto &[name, _] : impl_->command_refs) {
        names.push_back(name);
    }
#endif
    std::sort(names.begin(), names.end());
    return names;
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
