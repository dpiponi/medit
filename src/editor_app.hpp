#pragma once

#include "editor_state.hpp"

std::optional<std::string> open_startup_files(EditorState &state, int argc, char **argv);
void setup_terminal(const Theme &theme);
void teardown_terminal();
void render_frame(EditorState &state);
void run_editor(EditorState &state);
void initialize_locale();
bool suspend_supported();
void initialize_windows(EditorState &state);
