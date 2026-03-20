#include "command_recording.hpp"

#include "logger.hpp"

#include <string>

// Forward declaration from editor_input.cpp
void process_input_token(EditorState &state, const std::string &token, wint_t key, bool printable);
std::size_t take_repeat_count(EditorState &state);

std::string join_logged_tokens(const std::vector<std::string> &tokens) {
    std::string result;
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        if (!result.empty()) {
            result += ' ';
        }
        result += tokens[index];
    }
    return result;
}

std::vector<std::string> logged_tokens(const std::vector<EditorState::RecordedInput> &inputs) {
    std::vector<std::string> tokens;
    tokens.reserve(inputs.size());
    for (const EditorState::RecordedInput &input : inputs) {
        tokens.push_back(input.token);
    }
    return tokens;
}

namespace {

std::vector<std::pair<std::size_t, std::size_t>> capture_buffer_versions(const EditorState &state) {
    std::vector<std::pair<std::size_t, std::size_t>> versions;
    versions.reserve(state.session.buffers().size());
    for (const EditorBuffer &buffer : state.session.buffers()) {
        versions.push_back({buffer.id, buffer.core.document_version()});
    }
    return versions;
}

}  // namespace

// CommandRecordingState methods

bool EditorState::CommandRecordingState::can_start() const {
    return replay_depth == 0 && !recording;
}

void EditorState::CommandRecordingState::begin(const EditorState &state) {
    recording = true;
    recording_nonrepeatable = false;
    inputs.clear();
    buffer_versions = capture_buffer_versions(state);
}

void EditorState::CommandRecordingState::reset() {
    recording = false;
    recording_nonrepeatable = false;
    inputs.clear();
    buffer_versions.clear();
}

bool EditorState::CommandRecordingState::is_complete(const EditorState &state) const {
    if (!recording) {
        return false;
    }
    if (replay_depth > 0) {
        return false;
    }
    if (group_depth != 0 || !state.pending.tokens.empty() || state.pending.motion != PendingMotion::None ||
        state.pending.replace_count != 0) {
        return false;
    }
    return state.mode != Mode::Insert && state.mode != Mode::Command && state.mode != Mode::Search;
}

bool EditorState::CommandRecordingState::changed_buffer(const EditorState &state) const {
    std::vector<std::pair<std::size_t, std::size_t>> after = capture_buffer_versions(state);
    return after != buffer_versions;
}

void EditorState::CommandRecordingState::finalize(EditorState &state) {
    if (!is_complete(state)) {
        return;
    }
    bool changed = changed_buffer(state);
    if (!recording_nonrepeatable && changed && !inputs.empty()) {
        last_repeatable = inputs;
        log_debug("command recorded inputs=[" + join_logged_tokens(logged_tokens(inputs)) + "]");
    }
    reset();
}

void EditorState::CommandRecordingState::record_group_input(const std::string &token, wint_t key, bool printable) {
    if (group_depth == 0) {
        return;
    }
    group_inputs.push_back({token, key, printable});
}

void EditorState::CommandRecordingState::record_command_input(const std::string &token, wint_t key, bool printable) {
    if (!recording || replay_depth > 0) {
        return;
    }
    inputs.push_back({token, key, printable});
}

void EditorState::CommandRecordingState::replay_group_inputs(EditorState &state, const std::vector<EditorState::RecordedInput> &recorded_inputs, std::size_t repeat) {
    static constexpr std::size_t kMaxReplayDepth = 16;
    if (repeat <= 1) {
        return;
    }
    if (replay_depth >= kMaxReplayDepth) {
        state.set_status("Command group nesting too deep");
        return;
    }

    log_debug(
        "command group replay repeat=" + std::to_string(repeat) +
        " inputs=[" + join_logged_tokens(logged_tokens(recorded_inputs)) + "]");
    ++replay_depth;
    for (std::size_t iteration = 1; iteration < repeat; ++iteration) {
        for (const EditorState::RecordedInput &input : recorded_inputs) {
            process_input_token(state, input.token, input.key, input.printable);
        }
    }
    --replay_depth;
}

bool EditorState::CommandRecordingState::repeat_last_command(EditorState &state) {
    static constexpr std::size_t kMaxReplayDepth = 16;
    if (last_repeatable.empty()) {
        state.set_status("Nothing to repeat");
        return false;
    }
    if (replay_depth >= kMaxReplayDepth) {
        state.set_status("Repeat nesting too deep");
        return false;
    }

    std::size_t repeat = take_repeat_count(state);
    log_debug(
        "repeat command repeat=" + std::to_string(repeat) +
        " inputs=[" + join_logged_tokens(logged_tokens(last_repeatable)) + "]");
    state.active_core().begin_compound_edit();
    ++replay_depth;
    for (std::size_t iteration = 0; iteration < repeat; ++iteration) {
        for (const EditorState::RecordedInput &input : last_repeatable) {
            process_input_token(state, input.token, input.key, input.printable);
        }
    }
    --replay_depth;
    state.active_core().end_compound_edit();
    state.set_status("Repeated command");
    return true;
}

bool repeat_last_command(EditorState &state) {
    return state.recording.repeat_last_command(state);
}
