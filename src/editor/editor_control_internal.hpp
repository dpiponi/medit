#pragma once

#include "editor_state.hpp"

JsonValue json_position(Position position);
JsonValue json_range(const Range &range);
std::string buffer_display_name(const EditorBuffer &buffer);
std::string buffer_text_utf8(const EditorBuffer &buffer);
JsonValue json_buffer_summary(const EditorState &state, const EditorBuffer &buffer);
const char *selection_mode_name(SelectionMode mode);
std::string mode_name(Mode mode);
std::string prefixed_message(const char *prefix, const std::string &value);
void render_frame(EditorState &state);
void update_input_timeout(const EditorState &state);
std::string handle_control_request(EditorState &state, std::string_view request_text);
void run_editor(EditorState &state);
std::optional<std::string> open_startup_files(EditorState &state, int argc, char **argv);
