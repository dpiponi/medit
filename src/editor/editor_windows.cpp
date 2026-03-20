#include "editor_windows.hpp"

#include <algorithm>
#include <limits>

namespace {

bool ranges_overlap(int start_a, int end_a, int start_b, int end_b) {
    return std::max(start_a, start_b) < std::min(end_a, end_b);
}

int rect_center_x(const WindowLayoutRect &rect) {
    return rect.left + rect.width / 2;
}

int rect_center_y(const WindowLayoutRect &rect) {
    return rect.top + rect.height / 2;
}

} // namespace

WindowManager::WindowManager(std::size_t initial_buffer_id) {
    if (initial_buffer_id == 0) {
        return;
    }

    EditorWindow window;
    window.id = next_window_id_++;
    window.buffer_id = initial_buffer_id;
    windows_.push_back(window);
    active_window_id_ = window.id;

    LayoutNode root;
    root.id = next_node_id_++;
    root.is_leaf = true;
    root.window_id = window.id;
    nodes_.push_back(root);
    root_node_id_ = root.id;
}

std::size_t WindowManager::window_count() const {
    return windows_.size();
}

std::size_t WindowManager::active_window_id() const {
    return active_window_id_;
}

std::size_t WindowManager::active_window_index() const {
    for (std::size_t index = 0; index < windows_.size(); ++index) {
        if (windows_[index].id == active_window_id_) {
            return index;
        }
    }
    return 0;
}

bool WindowManager::set_active_window(std::size_t window_id) {
    if (!find_window(window_id)) {
        return false;
    }
    active_window_id_ = window_id;
    return true;
}

EditorWindow *WindowManager::active_window() {
    return find_window(active_window_id_);
}

const EditorWindow *WindowManager::active_window() const {
    return find_window(active_window_id_);
}

EditorWindow *WindowManager::find_window(std::size_t window_id) {
    for (EditorWindow &window : windows_) {
        if (window.id == window_id) {
            return &window;
        }
    }
    return nullptr;
}

const EditorWindow *WindowManager::find_window(std::size_t window_id) const {
    for (const EditorWindow &window : windows_) {
        if (window.id == window_id) {
            return &window;
        }
    }
    return nullptr;
}

std::optional<std::size_t> WindowManager::find_window_showing_buffer(std::size_t buffer_id) const {
    for (const EditorWindow &window : windows_) {
        if (window.buffer_id == buffer_id) {
            return window.id;
        }
    }
    return std::nullopt;
}

const std::vector<EditorWindow> &WindowManager::windows() const {
    return windows_;
}

void WindowManager::set_active_buffer_id(std::size_t buffer_id) {
    if (EditorWindow *window = active_window()) {
        window->buffer_id = buffer_id;
    }
}

void WindowManager::replace_buffer_id(std::size_t old_buffer_id, std::size_t new_buffer_id) {
    for (EditorWindow &window : windows_) {
        if (window.buffer_id == old_buffer_id) {
            window.buffer_id = new_buffer_id;
        }
    }
}

bool WindowManager::split_active(WindowSplitDirection direction) {
    EditorWindow *window = active_window();
    if (!window) {
        return false;
    }
    std::size_t current_window_id = window->id;
    std::size_t current_buffer_id = window->buffer_id;
    const LayoutNode *leaf = find_leaf_node_for_window(root_node_id_, window->id);
    if (!leaf) {
        return false;
    }
    std::size_t leaf_id = leaf->id;

    EditorWindow sibling;
    sibling.id = next_window_id_++;
    sibling.buffer_id = current_buffer_id;
    windows_.push_back(sibling);

    LayoutNode first_child;
    first_child.id = next_node_id_++;
    first_child.is_leaf = true;
    first_child.window_id = current_window_id;

    LayoutNode second_child;
    second_child.id = next_node_id_++;
    second_child.is_leaf = true;
    second_child.window_id = sibling.id;

    nodes_.push_back(first_child);
    nodes_.push_back(second_child);

    LayoutNode *mutable_leaf = find_node(leaf_id);
    if (!mutable_leaf) {
        return false;
    }

    mutable_leaf->is_leaf = false;
    mutable_leaf->direction = direction;
    mutable_leaf->window_id = 0;
    mutable_leaf->first_child_id = first_child.id;
    mutable_leaf->second_child_id = second_child.id;

    active_window_id_ = sibling.id;
    return true;
}

bool WindowManager::close_active() {
    if (windows_.size() <= 1) {
        return false;
    }

    std::size_t closing_window_id = active_window_id_;
    if (!close_window_in_node(root_node_id_, closing_window_id)) {
        return false;
    }

    windows_.erase(
        std::remove_if(
            windows_.begin(),
            windows_.end(),
            [closing_window_id](const EditorWindow &window) { return window.id == closing_window_id; }),
        windows_.end());

    if (!windows_.empty() && !find_window(active_window_id_)) {
        active_window_id_ = windows_.front().id;
    }
    return true;
}

bool WindowManager::close_others() {
    const EditorWindow *window = active_window();
    if (!window) {
        return false;
    }
    std::size_t keep_window_id = window->id;
    if (windows_.size() <= 1) {
        return false;
    }

    EditorWindow kept_window = *window;
    windows_.clear();
    windows_.push_back(std::move(kept_window));

    nodes_.clear();
    LayoutNode root;
    root.id = next_node_id_++;
    root.is_leaf = true;
    root.window_id = keep_window_id;
    nodes_.push_back(root);
    root_node_id_ = root.id;
    active_window_id_ = keep_window_id;
    return true;
}

bool WindowManager::focus_direction(WindowMoveDirection direction, int total_rows, int total_cols, int reserved_rows) {
    const EditorWindow *window = active_window();
    if (!window) {
        return false;
    }

    std::vector<WindowLayoutRect> rects = layout_rects(total_rows, total_cols, reserved_rows);
    const WindowLayoutRect *active_rect = nullptr;
    for (const WindowLayoutRect &rect : rects) {
        if (rect.window_id == window->id) {
            active_rect = &rect;
            break;
        }
    }
    if (!active_rect) {
        return false;
    }

    const WindowLayoutRect *best = nullptr;
    int best_primary = std::numeric_limits<int>::max();
    int best_secondary = std::numeric_limits<int>::max();
    for (const WindowLayoutRect &rect : rects) {
        if (rect.window_id == active_rect->window_id) {
            continue;
        }

        int primary_distance = std::numeric_limits<int>::max();
        int secondary_distance = std::numeric_limits<int>::max();
        bool valid = false;
        switch (direction) {
            case WindowMoveDirection::Left:
                valid = rect.left + rect.width <= active_rect->left;
                if (valid) {
                    primary_distance = active_rect->left - (rect.left + rect.width);
                    secondary_distance = std::abs(rect_center_y(rect) - rect_center_y(*active_rect));
                    if (ranges_overlap(rect.top, rect.top + rect.height, active_rect->top, active_rect->top + active_rect->height)) {
                        secondary_distance = 0;
                    }
                }
                break;
            case WindowMoveDirection::Right:
                valid = rect.left >= active_rect->left + active_rect->width;
                if (valid) {
                    primary_distance = rect.left - (active_rect->left + active_rect->width);
                    secondary_distance = std::abs(rect_center_y(rect) - rect_center_y(*active_rect));
                    if (ranges_overlap(rect.top, rect.top + rect.height, active_rect->top, active_rect->top + active_rect->height)) {
                        secondary_distance = 0;
                    }
                }
                break;
            case WindowMoveDirection::Up:
                valid = rect.top + rect.height <= active_rect->top;
                if (valid) {
                    primary_distance = active_rect->top - (rect.top + rect.height);
                    secondary_distance = std::abs(rect_center_x(rect) - rect_center_x(*active_rect));
                    if (ranges_overlap(rect.left, rect.left + rect.width, active_rect->left, active_rect->left + active_rect->width)) {
                        secondary_distance = 0;
                    }
                }
                break;
            case WindowMoveDirection::Down:
                valid = rect.top >= active_rect->top + active_rect->height;
                if (valid) {
                    primary_distance = rect.top - (active_rect->top + active_rect->height);
                    secondary_distance = std::abs(rect_center_x(rect) - rect_center_x(*active_rect));
                    if (ranges_overlap(rect.left, rect.left + rect.width, active_rect->left, active_rect->left + active_rect->width)) {
                        secondary_distance = 0;
                    }
                }
                break;
        }

        if (!valid) {
            continue;
        }
        if (!best || primary_distance < best_primary ||
            (primary_distance == best_primary && secondary_distance < best_secondary)) {
            best = &rect;
            best_primary = primary_distance;
            best_secondary = secondary_distance;
        }
    }

    if (!best) {
        return false;
    }
    active_window_id_ = best->window_id;
    return true;
}

std::vector<WindowLayoutRect> WindowManager::layout_rects(int total_rows, int total_cols, int reserved_rows) const {
    std::vector<WindowLayoutRect> rects;
    int usable_rows = total_rows - reserved_rows;
    if (root_node_id_ == 0 || total_cols <= 0 || usable_rows <= 0) {
        return rects;
    }
    collect_layout_rects(root_node_id_, 0, 0, usable_rows, total_cols, rects);
    return rects;
}

WindowManager::LayoutNode *WindowManager::find_node(std::size_t node_id) {
    for (LayoutNode &node : nodes_) {
        if (node.id == node_id) {
            return &node;
        }
    }
    return nullptr;
}

const WindowManager::LayoutNode *WindowManager::find_node(std::size_t node_id) const {
    for (const LayoutNode &node : nodes_) {
        if (node.id == node_id) {
            return &node;
        }
    }
    return nullptr;
}

const WindowManager::LayoutNode *WindowManager::find_leaf_node_for_window(std::size_t node_id, std::size_t window_id) const {
    const LayoutNode *node = find_node(node_id);
    if (!node) {
        return nullptr;
    }
    if (node->is_leaf) {
        return node->window_id == window_id ? node : nullptr;
    }
    if (const LayoutNode *first = find_leaf_node_for_window(node->first_child_id, window_id)) {
        return first;
    }
    return find_leaf_node_for_window(node->second_child_id, window_id);
}

bool WindowManager::close_window_in_node(std::size_t node_id, std::size_t target_window_id) {
    LayoutNode *node = find_node(node_id);
    if (!node || node->is_leaf) {
        return false;
    }

    LayoutNode *first = find_node(node->first_child_id);
    LayoutNode *second = find_node(node->second_child_id);
    if (!first || !second) {
        return false;
    }

    auto promote_sibling = [&](LayoutNode *sibling, std::size_t removed_child_id, std::size_t sibling_id) {
        LayoutNode promoted = *sibling;
        std::size_t preserved_id = node->id;
        *node = promoted;
        node->id = preserved_id;
        nodes_.erase(
            std::remove_if(
                nodes_.begin(),
                nodes_.end(),
                [removed_child_id, sibling_id](const LayoutNode &candidate) {
                    return candidate.id == removed_child_id || candidate.id == sibling_id;
                }),
            nodes_.end());
    };

    if (first->is_leaf && first->window_id == target_window_id) {
        active_window_id_ = second->is_leaf ? second->window_id : target_window_id;
        promote_sibling(second, first->id, second->id);
        return true;
    }
    if (second->is_leaf && second->window_id == target_window_id) {
        active_window_id_ = first->is_leaf ? first->window_id : target_window_id;
        promote_sibling(first, second->id, first->id);
        return true;
    }
    if (!first->is_leaf && close_window_in_node(first->id, target_window_id)) {
        return true;
    }
    if (!second->is_leaf && close_window_in_node(second->id, target_window_id)) {
        return true;
    }
    return false;
}

void WindowManager::collect_layout_rects(
    std::size_t node_id,
    int top,
    int left,
    int height,
    int width,
    std::vector<WindowLayoutRect> &rects) const {
    const LayoutNode *node = find_node(node_id);
    if (!node || height <= 0 || width <= 0) {
        return;
    }
    if (node->is_leaf) {
        rects.push_back({node->window_id, top, left, height, width});
        return;
    }

    if (node->direction == WindowSplitDirection::Vertical) {
        int left_width = std::max(1, (width - 1) / 2);
        int right_width = std::max(1, width - 1 - left_width);
        collect_layout_rects(node->first_child_id, top, left, height, left_width, rects);
        collect_layout_rects(node->second_child_id, top, left + left_width + 1, height, right_width, rects);
        return;
    }

    int top_height = std::max(1, (height - 1) / 2);
    int bottom_height = std::max(1, height - 1 - top_height);
    collect_layout_rects(node->first_child_id, top, left, top_height, width, rects);
    collect_layout_rects(node->second_child_id, top + top_height + 1, left, bottom_height, width, rects);
}
