#include "position_utils.hpp"

#include <algorithm>

// Wrapper functions for backward compatibility
// These now use Position's built-in operators
bool position_less_than(Position left, Position right) {
    return left < right;
}

bool positions_equal(Position left, Position right) {
    return left == right;
}

Range normalized_range(Range range) {
    if (range.end < range.start) {
        std::swap(range.start, range.end);
    }
    return range;
}

bool range_contains(const Range &range, Position position) {
    Range normalized = normalized_range(range);
    return !(position < normalized.start) && position < normalized.end;
}

bool ranges_overlap(const Range &left, const Range &right) {
    Range normalized_left = normalized_range(left);
    Range normalized_right = normalized_range(right);
    return normalized_left.start < normalized_right.end &&
           normalized_right.start < normalized_left.end;
}

Range full_document_range(const Lines &lines) {
    if (lines.empty()) {
        return {{0, 0}, {0, 0}};
    }
    std::size_t last_row = lines.size() - 1;
    return {{0, 0}, {last_row, lines[last_row].size()}};
}
