#pragma once

#include <string>

// UTF-8 to UTF-32 conversion utilities

std::u32string utf8_to_u32(const std::string &text);
std::string u32_to_utf8(const std::u32string &text);
