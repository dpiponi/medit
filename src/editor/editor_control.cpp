#include "editor_control_internal.hpp"

#include "json.hpp"
#include "string_utils.hpp"

JsonValue success_control_result(JsonValue result) {
    return JsonValue{{"ok", true}, {"result", std::move(result)}};
}

JsonValue error_control_result(const std::string &message) {
    return JsonValue{{"ok", false}, {"error", message}};
}

std::optional<std::size_t> parse_optional_buffer_id(const JsonValue &params) {
    auto found = params.find("buffer_id");
    if (found == params.end() || found->is_null()) {
        return std::nullopt;
    }
    if (!found->is_number_unsigned()) {
        throw std::runtime_error("buffer_id must be an unsigned integer");
    }
    return found->get<std::size_t>();
}

std::optional<std::size_t> parse_optional_window_id(const JsonValue &params) {
    auto found = params.find("window_id");
    if (found == params.end() || found->is_null()) {
        return std::nullopt;
    }
    if (!found->is_number_unsigned()) {
        throw std::runtime_error("window_id must be an unsigned integer");
    }
    return found->get<std::size_t>();
}

std::optional<std::size_t> parse_optional_row_param(const JsonValue &params, const char *name) {
    auto found = params.find(name);
    if (found == params.end() || found->is_null()) {
        return std::nullopt;
    }
    if (!found->is_number_unsigned()) {
        throw std::runtime_error(std::string(name) + " must be an unsigned integer");
    }
    return found->get<std::size_t>();
}

EditorBuffer *control_target_buffer(EditorState &state, const JsonValue &params) {
    std::optional<std::size_t> buffer_id = parse_optional_buffer_id(params);
    if (!buffer_id) {
        return &state.active_buffer();
    }
    return state.session.find_buffer_by_id(*buffer_id);
}

// Generic template for parsing JSON string to enum with validation
template<typename EnumType>
EnumType json_to_enum(const JsonValue &value,
                      const std::string &type_name,
                      const std::map<std::string, EnumType> &mapping) {
    if (!value.is_string()) {
        throw std::runtime_error(type_name + " must be a string");
    }
    const std::string str_value = value.get<std::string>();
    auto it = mapping.find(str_value);
    if (it != mapping.end()) {
        return it->second;
    }

    // Build error message with valid options
    std::string valid_options;
    for (const auto &[key, _] : mapping) {
        if (!valid_options.empty()) {
            valid_options += "' or '";
        }
        valid_options += key;
    }
    throw std::runtime_error(type_name + " must be '" + valid_options + "'");
}

SelectionMode json_to_selection_mode(const JsonValue &value) {
    static const std::map<std::string, SelectionMode> mapping = {
        {"character", SelectionMode::Character},
        {"line", SelectionMode::Line}
    };
    return json_to_enum(value, "selection mode", mapping);
}

WindowSplitDirection json_to_split_direction(const JsonValue &value) {
    static const std::map<std::string, WindowSplitDirection> mapping = {
        {"horizontal", WindowSplitDirection::Horizontal},
        {"vertical", WindowSplitDirection::Vertical}
    };
    return json_to_enum(value, "direction", mapping);
}

std::string json_to_open_line_direction(const JsonValue &value) {
    if (!value.is_string()) {
        throw std::runtime_error("direction must be a string");
    }
    const std::string direction = value.get<std::string>();
    if (direction != "above" && direction != "below") {
        throw std::runtime_error("direction must be 'above' or 'below'");
    }
    return direction;
}

Position json_to_position(const JsonValue &value) {
    if (!value.is_object()) {
        throw std::runtime_error("position must be an object");
    }
    auto row = value.find("row");
    auto column = value.find("column");
    if (row == value.end() || column == value.end() || !row->is_number_unsigned() || !column->is_number_unsigned()) {
        throw std::runtime_error("position must contain unsigned row and column");
    }
    return {row->get<std::size_t>(), column->get<std::size_t>()};
}

Range json_to_range(const JsonValue &value) {
    if (!value.is_object()) {
        throw std::runtime_error("range must be an object");
    }
    auto start = value.find("start");
    auto end = value.find("end");
    if (start == value.end() || end == value.end()) {
        throw std::runtime_error("range must contain start and end");
    }
    return {json_to_position(*start), json_to_position(*end)};
}

std::string handle_control_request(EditorState &state, std::string_view request_text) {
    try {
        JsonValue request = parse_json(std::string(request_text));
        if (!request.is_object()) {
            return error_control_result("request must be an object").dump();
        }
        std::string method = request.value("method", "");
        JsonValue params = request.contains("params") ? request["params"] : JsonValue::object();
        if (!params.is_object()) {
            return error_control_result("params must be an object").dump();
        }

        if (method == "status") {
            JsonValue result = {
                {"active_buffer_id", state.active_buffer().id},
                {"active_window_id", state.windows.active_window_id()},
                {"buffer_count", state.session.buffer_count()},
                {"window_count", state.windows.window_count()},
                {"mode", mode_name(state.mode)},
                {"status_message", state.status_message},
                {"control_socket", state.config.control_socket_path ? JsonValue(state.config.control_socket_path->string())
                                                                   : JsonValue(nullptr)},
                {"runtime", state.runtime.status_summary()},
            };
            return success_control_result(std::move(result)).dump();
        }

        if (method == "list_buffers") {
            JsonValue buffers = JsonValue::array();
            for (const EditorBuffer &buffer : state.session.buffers()) {
                buffers.push_back(json_buffer_summary(state, buffer));
            }
            return success_control_result(JsonValue{{"buffers", std::move(buffers)}}).dump();
        }

        if (method == "list_windows") {
            JsonValue windows = JsonValue::array();
            for (const EditorWindow &window : state.windows.windows()) {
                windows.push_back(
                    {{"id", window.id},
                     {"buffer_id", window.buffer_id},
                     {"active", window.id == state.windows.active_window_id()}});
            }
            return success_control_result(JsonValue{{"windows", std::move(windows)}}).dump();
        }

        if (method == "get_buffer") {
            EditorBuffer *buffer = control_target_buffer(state, params);
            if (!buffer) {
                return error_control_result("buffer not found").dump();
            }
            JsonValue result = json_buffer_summary(state, *buffer);
            result["text"] = buffer_text_utf8(*buffer);
            return success_control_result(std::move(result)).dump();
        }

        if (method == "get_lines") {
            EditorBuffer *buffer = control_target_buffer(state, params);
            if (!buffer) {
                return error_control_result("buffer not found").dump();
            }
            const std::size_t total_lines = buffer->core.line_count();
            const std::size_t start_row = parse_optional_row_param(params, "start_row").value_or(0);
            const std::size_t end_row = parse_optional_row_param(params, "end_row").value_or(total_lines);
            if (start_row > total_lines) {
                return error_control_result("start_row is out of range").dump();
            }
            if (end_row > total_lines) {
                return error_control_result("end_row is out of range").dump();
            }
            if (end_row < start_row) {
                return error_control_result("end_row must be greater than or equal to start_row").dump();
            }

            JsonValue lines = JsonValue::array();
            for (std::size_t row = start_row; row < end_row; ++row) {
                lines.push_back(JsonValue{
                    {"row", row},
                    {"text", u32_to_utf8(buffer->core.lines()[row])},
                });
            }
            return success_control_result(std::move(lines)).dump();
        }

        if (method == "get_selection") {
            std::optional<Range> selection = state.displayed_selection_range(state.windows.active_window_id());
            if (!selection) {
                return success_control_result(
                           JsonValue{{"selection", nullptr}, {"selection_mode", nullptr}, {"text", ""}})
                    .dump();
            }
            std::u32string text = state.active_core().read_text(*selection);
            return success_control_result(JsonValue{
                {"selection", json_range(*selection)},
                {"selection_mode", selection_mode_name(state.active_core().selection_mode())},
                {"text", u32_to_utf8(text)},
            })
                .dump();
        }

        if (method == "get_cursor") {
            std::optional<Range> selection = state.displayed_selection_range(state.windows.active_window_id());
            JsonValue result = {
                {"window_id", state.windows.active_window_id()},
                {"buffer_id", state.active_window().buffer_id},
                {"cursor", json_position(state.displayed_cursor(state.windows.active_window_id()))},
                {"selection", selection ? json_range(*selection) : JsonValue(nullptr)},
                {"selection_mode", selection ? JsonValue(selection_mode_name(state.active_core().selection_mode()))
                                             : JsonValue(nullptr)},
            };
            return success_control_result(std::move(result)).dump();
        }

        if (method == "set_cursor") {
            EditorBuffer *buffer = control_target_buffer(state, params);
            auto position = params.find("position");
            if (!buffer) {
                return error_control_result("buffer not found").dump();
            }
            if (position == params.end()) {
                return error_control_result("position is required").dump();
            }
            buffer->core.set_cursor(json_to_position(*position));
            if (buffer->id == state.active_window().buffer_id) {
                state.sync_window_view_from_core(state.windows.active_window_id());
            }
            return success_control_result(json_buffer_summary(state, *buffer)).dump();
        }

        if (method == "set_selection") {
            EditorBuffer *buffer = control_target_buffer(state, params);
            auto range = params.find("range");
            auto mode = params.find("mode");
            if (!buffer) {
                return error_control_result("buffer not found").dump();
            }
            if (range == params.end()) {
                return error_control_result("range is required").dump();
            }
            SelectionMode selection_mode = SelectionMode::Character;
            if (mode != params.end()) {
                selection_mode = json_to_selection_mode(*mode);
            }
            if (!buffer->core.set_selection_range(json_to_range(*range), selection_mode)) {
                return error_control_result("failed to set selection").dump();
            }
            if (buffer->id == state.active_window().buffer_id) {
                state.sync_window_view_from_core(state.windows.active_window_id());
            }
            return success_control_result(json_buffer_summary(state, *buffer)).dump();
        }

        if (method == "clear_selection") {
            EditorBuffer *buffer = control_target_buffer(state, params);
            if (!buffer) {
                return error_control_result("buffer not found").dump();
            }
            buffer->core.clear_selection();
            if (buffer->id == state.active_window().buffer_id) {
                state.sync_window_view_from_core(state.windows.active_window_id());
            }
            return success_control_result(json_buffer_summary(state, *buffer)).dump();
        }

        if (method == "open_file") {
            std::string path = params.value("path", "");
            if (path.empty()) {
                return error_control_result("path is required").dump();
            }
            EditorBuffer *buffer = state.session.open_file(path, true);
            if (!buffer) {
                return error_control_result("could not open file").dump();
            }
            state.show_buffer_in_active_window(buffer->id);
            return success_control_result(json_buffer_summary(state, *buffer)).dump();
        }

        if (method == "switch_buffer") {
            std::optional<std::size_t> buffer_id = parse_optional_buffer_id(params);
            if (!buffer_id) {
                return error_control_result("buffer_id is required").dump();
            }
            if (!state.session.switch_to_id(*buffer_id)) {
                return error_control_result("buffer not found").dump();
            }
            state.show_buffer_in_active_window(*buffer_id);
            state.set_status(prefixed_message("Switched to ", buffer_display_name(state.active_buffer())));
            return success_control_result(json_buffer_summary(state, state.active_buffer())).dump();
        }

        if (method == "apply_text_edits") {
            EditorBuffer *buffer = control_target_buffer(state, params);
            if (!buffer) {
                return error_control_result("buffer not found").dump();
            }
            auto edits = params.find("edits");
            if (edits == params.end() || !edits->is_array()) {
                return error_control_result("edits array is required").dump();
            }
            std::vector<TextEdit> parsed_edits;
            for (const JsonValue &edit : *edits) {
                if (!edit.is_object()) {
                    return error_control_result("edit entries must be objects").dump();
                }
                auto range = edit.find("range");
                auto text = edit.find("text");
                if (range == edit.end() || text == edit.end() || !text->is_string()) {
                    return error_control_result("each edit must contain range and text").dump();
                }
                parsed_edits.push_back({json_to_range(*range), utf8_to_u32(text->get<std::string>())});
            }
            if (!buffer->core.apply_text_edits(parsed_edits)) {
                return error_control_result("failed to apply edits").dump();
            }
            if (buffer->id == state.active_buffer().id) {
                state.sync_window_view_from_core(state.windows.active_window_id());
            }
            return success_control_result(json_buffer_summary(state, *buffer)).dump();
        }

        if (method == "open_line") {
            EditorBuffer *buffer = control_target_buffer(state, params);
            auto direction = params.find("direction");
            if (!buffer) {
                return error_control_result("buffer not found").dump();
            }
            if (direction == params.end()) {
                return error_control_result("direction is required").dump();
            }
            const std::string open_direction = json_to_open_line_direction(*direction);
            bool autoindent = effective_autoindent(state.config, buffer->core.file_path());
            auto autoindent_value = params.find("autoindent");
            if (autoindent_value != params.end()) {
                if (!autoindent_value->is_boolean()) {
                    return error_control_result("autoindent must be a boolean").dump();
                }
                autoindent = autoindent_value->get<bool>();
            }
            if (open_direction == "below") {
                if (autoindent) {
                    buffer->core.open_line_below_with_autoindent();
                } else {
                    buffer->core.open_line_below();
                }
            } else {
                if (autoindent) {
                    buffer->core.open_line_above_with_autoindent();
                } else {
                    buffer->core.open_line_above();
                }
            }
            if (buffer->id == state.active_window().buffer_id) {
                state.sync_window_view_from_core(state.windows.active_window_id());
            }
            return success_control_result(json_buffer_summary(state, *buffer)).dump();
        }

        if (method == "close_buffer") {
            std::optional<std::size_t> buffer_id = parse_optional_buffer_id(params);
            const bool force = params.value("force", false);
            const std::size_t target_buffer_id = buffer_id.value_or(state.active_window().buffer_id);
            EditorBuffer *target = state.session.find_buffer_by_id(target_buffer_id);
            if (!target) {
                return error_control_result("buffer not found").dump();
            }
            const std::string closed_name = buffer_display_name(*target);
            state.session.switch_to_id(target_buffer_id);
            EditorEvents closed_events;
            if (!state.session.close_active_buffer(force, &closed_events)) {
                return error_control_result("unsaved changes; pass force=true to close").dump();
            }
            for (const EditorEvent &event : closed_events) {
                state.dispatch_editor_event(event);
            }
            state.buffer_ui_map.erase(target_buffer_id);
            state.syntax_ui_map.erase(target_buffer_id);
            const std::size_t replacement_buffer_id = state.session.active_buffer_id();
            state.windows.replace_buffer_id(target_buffer_id, replacement_buffer_id);
            for (const EditorWindow &window : state.windows.windows()) {
                if (window.buffer_id == replacement_buffer_id) {
                    state.window_ui(window.id) = EditorState::WindowUiState{};
                }
            }
            state.sync_active_window_buffer();
            state.active_buffer_ui();
            state.set_status(prefixed_message("Closed ", closed_name));
            return success_control_result(JsonValue{
                {"closed_buffer_id", target_buffer_id},
                {"active_buffer", json_buffer_summary(state, state.active_buffer())},
            })
                .dump();
        }

        if (method == "focus_window") {
            std::optional<std::size_t> window_id = parse_optional_window_id(params);
            if (!window_id) {
                return error_control_result("window_id is required").dump();
            }
            if (!state.windows.find_window(*window_id)) {
                return error_control_result("window not found").dump();
            }
            state.focus_window(*window_id);
            return success_control_result(JsonValue{
                {"window_id", state.windows.active_window_id()},
                {"buffer", json_buffer_summary(state, state.active_buffer())},
            })
                .dump();
        }

        if (method == "split_window") {
            auto direction = params.find("direction");
            if (direction == params.end()) {
                return error_control_result("direction is required").dump();
            }
            if (!state.split_active_window(json_to_split_direction(*direction))) {
                return error_control_result("could not split window").dump();
            }
            return success_control_result(JsonValue{
                {"window_id", state.windows.active_window_id()},
                {"buffer", json_buffer_summary(state, state.active_buffer())},
            })
                .dump();
        }

        if (method == "close_window") {
            std::optional<std::size_t> window_id = parse_optional_window_id(params);
            const std::size_t closing_window_id = window_id.value_or(state.windows.active_window_id());
            if (!state.windows.find_window(closing_window_id)) {
                return error_control_result("window not found").dump();
            }
            state.focus_window(closing_window_id);
            if (!state.close_active_window() && !state.should_quit) {
                return error_control_result("could not close window").dump();
            }
            return success_control_result(JsonValue{
                {"closed_window_id", closing_window_id},
                {"active_window_id", state.windows.active_window_id()},
                {"window_count", state.windows.window_count()},
                {"should_quit", state.should_quit},
            })
                .dump();
        }

        if (method == "close_other_windows") {
            if (!state.close_other_windows()) {
                return error_control_result("no other windows").dump();
            }
            return success_control_result(JsonValue{
                {"active_window_id", state.windows.active_window_id()},
                {"window_count", state.windows.window_count()},
                {"buffer", json_buffer_summary(state, state.active_buffer())},
            })
                .dump();
        }

        if (method == "save_buffer") {
            EditorBuffer *buffer = control_target_buffer(state, params);
            if (!buffer) {
                return error_control_result("buffer not found").dump();
            }
            bool ok = false;
            std::string path = params.value("path", "");
            if (!path.empty()) {
                ok = buffer->core.save_current_file_as(path);
            } else {
                ok = buffer->core.save_current_file();
            }
            if (!ok) {
                return error_control_result(buffer->core.file_path() ? "save failed" : "no file name").dump();
            }
            return success_control_result(json_buffer_summary(state, *buffer)).dump();
        }

        return error_control_result("unknown control method: " + method).dump();
    } catch (const std::exception &error) {
        return error_control_result(error.what()).dump();
    }
}

void run_editor(EditorState &state) {
    while (!state.should_quit) {
        for (EditorBuffer &buffer : state.session.buffers()) {
            state.dispatch_editor_events(buffer.core);
        }
        state.runtime.poll_services();
        state.handle_service_events();
        state.lua.poll_async(state);
        state.control_server.poll([&state](std::string_view request) { return handle_control_request(state, request); });
        render_frame(state);
        update_input_timeout(state);
        state.handle_input();
    }
}

std::optional<std::string> open_startup_files(EditorState &state, int argc, char **argv) {
    if (argc <= 1) {
        return std::nullopt;
    }

    if (argc == 2) {
        std::filesystem::path candidate = expand_user_path(argv[1]);
        if (std::filesystem::is_directory(candidate)) {
            std::filesystem::path normalized = std::filesystem::absolute(candidate).lexically_normal();
            state.set_status("Pick file from " + normalized.string());
            return normalized.string();
        }
    }

    std::string last_failure;
    for (int index = 1; index < argc; ++index) {
        std::string path = expand_user_path(argv[index]);
        EditorBuffer *buffer = state.session.open_file(path, index == 1);
        if (buffer) {
            if (index == 1) {
                state.show_buffer_in_active_window(buffer->id);
            }
            state.set_status("Opened " + path);
        } else {
            last_failure = "Could not open file: " + path;
        }
    }

    state.active_buffer_ui();
    if (!last_failure.empty()) {
        state.set_status(last_failure);
    }
    return std::nullopt;
}
