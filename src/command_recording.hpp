#pragma once

#include "editor_internal.hpp"

#include <string>
#include <vector>

// Command recording and replay functionality

// Repeat the last recorded command
bool repeat_last_command(EditorState &state);

// Helper functions for debug logging
std::string join_logged_tokens(const std::vector<std::string> &tokens);
std::vector<std::string> logged_tokens(const std::vector<EditorState::RecordedInput> &inputs);
