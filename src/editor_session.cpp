#include "editor_session.hpp"

#include <filesystem>
#include <utility>

EditorSession::EditorSession() {
    new_buffer(true);
    configure_clipboard(default_clipboard_config());
}

std::size_t EditorSession::buffer_count() const {
    return buffers_.size();
}

std::size_t EditorSession::active_buffer_index() const {
    return active_buffer_index_;
}

std::size_t EditorSession::active_buffer_id() const {
    return active_buffer().id;
}

EditorBuffer &EditorSession::active_buffer() {
    return buffers_.at(active_buffer_index_);
}

const EditorBuffer &EditorSession::active_buffer() const {
    return buffers_.at(active_buffer_index_);
}

std::vector<EditorBuffer> &EditorSession::buffers() {
    return buffers_;
}

const std::vector<EditorBuffer> &EditorSession::buffers() const {
    return buffers_;
}

bool EditorSession::has_dirty_buffers() const {
    for (const EditorBuffer &buffer : buffers_) {
        if (buffer.core.is_dirty()) {
            return true;
        }
    }
    return false;
}

ClipboardSnapshot EditorSession::clipboard() const {
    return clipboard_;
}

void EditorSession::configure_clipboard(ClipboardConfig config) {
    clipboard_provider_ = std::make_unique<ClipboardProvider>(std::move(config));
    clipboard_ = clipboard_provider_->read(clipboard_);
    sync_clipboard_into_all_buffers();
}

void EditorSession::set_clipboard(std::u32string text, SelectionMode mode) {
    clipboard_.text = std::move(text);
    clipboard_.mode = mode;
    if (clipboard_provider_) {
        clipboard_provider_->write(clipboard_);
    }
    sync_active_clipboard();
}

void EditorSession::capture_active_clipboard() {
    clipboard_.text = active_buffer().core.yank_buffer();
    clipboard_.mode = active_buffer().core.yank_mode();
    if (clipboard_provider_) {
        clipboard_provider_->write(clipboard_);
    }
}

void EditorSession::sync_active_clipboard() {
    if (clipboard_provider_) {
        clipboard_ = clipboard_provider_->read(clipboard_);
    }
    sync_clipboard_into(active_buffer());
}

EditorBuffer &EditorSession::new_buffer(bool activate) {
    return create_buffer(activate);
}

EditorBuffer *EditorSession::open_file(const std::string &path, bool activate) {
    EditorBuffer &buffer = create_buffer(activate);
    if (!buffer.core.load_file(path)) {
        if (std::filesystem::exists(path)) {
            std::size_t opened_index = buffers_.size() - 1;
            buffers_.erase(buffers_.begin() + static_cast<std::ptrdiff_t>(opened_index));
            if (buffers_.empty()) {
                new_buffer(true);
            } else if (active_buffer_index_ >= buffers_.size()) {
                active_buffer_index_ = buffers_.size() - 1;
            }
            return nullptr;
        }
        buffer.core.open_empty_file(path);
    }
    sync_clipboard_into(buffer);
    return &buffer;
}

bool EditorSession::switch_to_index(std::size_t index) {
    if (index >= buffers_.size()) {
        return false;
    }
    active_buffer_index_ = index;
    sync_active_clipboard();
    return true;
}

bool EditorSession::switch_to_id(std::size_t id) {
    for (std::size_t index = 0; index < buffers_.size(); ++index) {
        if (buffers_[index].id == id) {
            return switch_to_index(index);
        }
    }
    return false;
}

bool EditorSession::next_buffer() {
    if (buffers_.empty()) {
        return false;
    }
    return switch_to_index((active_buffer_index_ + 1) % buffers_.size());
}

bool EditorSession::previous_buffer() {
    if (buffers_.empty()) {
        return false;
    }
    std::size_t next_index = active_buffer_index_ == 0 ? buffers_.size() - 1 : active_buffer_index_ - 1;
    return switch_to_index(next_index);
}

bool EditorSession::close_active_buffer(bool force, EditorEvents *closed_events) {
    if (buffers_.empty()) {
        return false;
    }
    if (!force && active_buffer().core.is_dirty()) {
        return false;
    }

    active_buffer().core.close_document();
    if (closed_events) {
        EditorEvents events = active_buffer().core.take_events();
        closed_events->insert(
            closed_events->end(),
            std::make_move_iterator(events.begin()),
            std::make_move_iterator(events.end()));
    }

    buffers_.erase(buffers_.begin() + static_cast<std::ptrdiff_t>(active_buffer_index_));
    if (buffers_.empty()) {
        new_buffer(true);
        return true;
    }
    if (active_buffer_index_ >= buffers_.size()) {
        active_buffer_index_ = buffers_.size() - 1;
    }
    sync_active_clipboard();
    return true;
}

EditorBuffer *EditorSession::find_buffer_by_id(std::size_t id) {
    for (EditorBuffer &buffer : buffers_) {
        if (buffer.id == id) {
            return &buffer;
        }
    }
    return nullptr;
}

const EditorBuffer *EditorSession::find_buffer_by_id(std::size_t id) const {
    for (const EditorBuffer &buffer : buffers_) {
        if (buffer.id == id) {
            return &buffer;
        }
    }
    return nullptr;
}

EditorBuffer *EditorSession::find_buffer_by_uri(const std::string &document_uri) {
    for (EditorBuffer &buffer : buffers_) {
        if (buffer.core.document_uri() == document_uri) {
            return &buffer;
        }
    }
    return nullptr;
}

const EditorBuffer *EditorSession::find_buffer_by_uri(const std::string &document_uri) const {
    for (const EditorBuffer &buffer : buffers_) {
        if (buffer.core.document_uri() == document_uri) {
            return &buffer;
        }
    }
    return nullptr;
}

std::optional<std::size_t> EditorSession::index_for_buffer_id(std::size_t id) const {
    for (std::size_t index = 0; index < buffers_.size(); ++index) {
        if (buffers_[index].id == id) {
            return index;
        }
    }
    return std::nullopt;
}

EditorBuffer &EditorSession::create_buffer(bool activate) {
    EditorBuffer buffer;
    buffer.id = next_buffer_id_++;
    sync_clipboard_into(buffer);
    buffers_.push_back(std::move(buffer));
    if (activate) {
        active_buffer_index_ = buffers_.size() - 1;
    }
    return buffers_.back();
}

void EditorSession::sync_clipboard_into(EditorBuffer &buffer) {
    buffer.core.set_yank_buffer(clipboard_.text, clipboard_.mode);
}

void EditorSession::sync_clipboard_into_all_buffers() {
    for (EditorBuffer &buffer : buffers_) {
        sync_clipboard_into(buffer);
    }
}
