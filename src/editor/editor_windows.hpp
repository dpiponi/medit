#pragma once

#include <cstddef>
#include <optional>
#include <vector>

enum class WindowSplitDirection {
    Horizontal,
    Vertical,
};

enum class WindowMoveDirection {
    Left,
    Right,
    Up,
    Down,
};

struct EditorWindow {
    std::size_t id = 0;
    std::size_t buffer_id = 0;
};

struct WindowLayoutRect {
    std::size_t window_id = 0;
    int top = 0;
    int left = 0;
    int height = 0;
    int width = 0;
};

class WindowManager {
  public:
    explicit WindowManager(std::size_t initial_buffer_id = 0);

    std::size_t window_count() const;
    std::size_t active_window_id() const;
    std::size_t active_window_index() const;
    bool set_active_window(std::size_t window_id);

    EditorWindow *active_window();
    const EditorWindow *active_window() const;
    EditorWindow *find_window(std::size_t window_id);
    const EditorWindow *find_window(std::size_t window_id) const;
    std::optional<std::size_t> find_window_showing_buffer(std::size_t buffer_id) const;

    const std::vector<EditorWindow> &windows() const;

    void set_active_buffer_id(std::size_t buffer_id);
    void replace_buffer_id(std::size_t old_buffer_id, std::size_t new_buffer_id);

    bool split_active(WindowSplitDirection direction);
    bool close_active();
    bool close_others();
    bool focus_direction(WindowMoveDirection direction, int total_rows, int total_cols, int reserved_rows);

    std::vector<WindowLayoutRect> layout_rects(int total_rows, int total_cols, int reserved_rows) const;

  private:
    struct LayoutNode {
        std::size_t id = 0;
        bool is_leaf = true;
        std::size_t window_id = 0;
        WindowSplitDirection direction = WindowSplitDirection::Vertical;
        std::size_t first_child_id = 0;
        std::size_t second_child_id = 0;
    };

    std::vector<EditorWindow> windows_;
    std::vector<LayoutNode> nodes_;
    std::size_t active_window_id_ = 0;
    std::size_t root_node_id_ = 0;
    std::size_t next_window_id_ = 1;
    std::size_t next_node_id_ = 1;

    LayoutNode *find_node(std::size_t node_id);
    const LayoutNode *find_node(std::size_t node_id) const;
    const LayoutNode *find_leaf_node_for_window(std::size_t node_id, std::size_t window_id) const;
    bool close_window_in_node(std::size_t node_id, std::size_t target_window_id);
    void collect_layout_rects(
        std::size_t node_id,
        int top,
        int left,
        int height,
        int width,
        std::vector<WindowLayoutRect> &rects) const;
};
