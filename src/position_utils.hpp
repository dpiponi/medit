#pragma once

#include "editor_core.hpp"

#include <vector>

// Position and Range utility functions

bool position_less_than(Position left, Position right);
bool positions_equal(Position left, Position right);
Range normalized_range(Range range);
bool range_contains(const Range &range, Position position);
bool ranges_overlap(const Range &left, const Range &right);
Range full_document_range(const std::vector<std::u32string> &lines);
