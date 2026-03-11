#pragma once

#include <cstddef>
#include <string>

std::string trim_ascii_whitespace(const std::string &value);
std::string ascii_lowercase(std::string value);
std::string normalize_extension(std::string extension);
std::string ellipsize_middle(const std::string &text, std::size_t max_width);
