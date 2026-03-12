#pragma once

#include "clipboard.hpp"
#include "editor_core.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct EditorBuffer {
    std::size_t id = 0;
    EditorCore core;
};

class EditorSession {
  public:
    EditorSession();

    std::size_t buffer_count() const;
    std::size_t active_buffer_index() const;
    std::size_t active_buffer_id() const;
    EditorBuffer &active_buffer();
    const EditorBuffer &active_buffer() const;
    std::vector<EditorBuffer> &buffers();
    const std::vector<EditorBuffer> &buffers() const;
    bool has_dirty_buffers() const;

    ClipboardSnapshot clipboard() const;
    void configure_clipboard(ClipboardConfig config);
    void set_clipboard(std::u32string text, SelectionMode mode);
    void capture_active_clipboard();
    void sync_active_clipboard();

    EditorBuffer &new_buffer(bool activate = true);
    EditorBuffer *open_file(const std::string &path, bool activate = true);
    bool switch_to_index(std::size_t index);
    bool switch_to_id(std::size_t id);
    bool next_buffer();
    bool previous_buffer();
    bool close_active_buffer(bool force, std::vector<EditorEvent> *closed_events = nullptr);

    EditorBuffer *find_buffer_by_id(std::size_t id);
    const EditorBuffer *find_buffer_by_id(std::size_t id) const;
    EditorBuffer *find_buffer_by_uri(const std::string &document_uri);
    const EditorBuffer *find_buffer_by_uri(const std::string &document_uri) const;
    std::optional<std::size_t> index_for_buffer_id(std::size_t id) const;

  private:
    std::vector<EditorBuffer> buffers_;
    std::size_t active_buffer_index_ = 0;
    std::size_t next_buffer_id_ = 1;
    ClipboardSnapshot clipboard_;
    std::unique_ptr<ClipboardProvider> clipboard_provider_;

    EditorBuffer &create_buffer(bool activate);
    void sync_clipboard_into(EditorBuffer &buffer);
    void sync_clipboard_into_all_buffers();
};
