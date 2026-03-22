#pragma once

import editor_core;

#include <filesystem>
#include <memory>
#include <optional>
#include <utility>
#include <string>
#include <vector>

struct EditorState;

class LuaRuntime {
  public:
    LuaRuntime();
    ~LuaRuntime();

    LuaRuntime(const LuaRuntime &) = delete;
    LuaRuntime &operator=(const LuaRuntime &) = delete;

    bool available() const;
    bool enabled() const;
    bool initialize(
        EditorState &state,
        const std::optional<std::filesystem::path> &script_path,
        std::string &error_message);
    void shutdown();
    bool execute_command(
        EditorState &state,
        const std::string &name,
        const std::string &argument,
        std::string &error_message);
    void detach_async_buffer(std::size_t buffer_id);
    void poll_async(EditorState &state);
    std::optional<int> idle_wait_timeout_ms() const;
    void dispatch_editor_event(EditorState &state, const EditorEvent &event);
    std::vector<std::string> registered_commands() const;
    std::vector<std::pair<std::string, std::string>> run_health_checks(EditorState &state) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
