#pragma once

#include <cstddef>
#include <string>
#include <string_view>

std::string trim_ascii_whitespace(std::string_view value);
std::string ascii_lowercase(std::string_view value);
std::string normalize_extension(std::string_view extension);
std::string ellipsize_middle(std::string_view text, std::size_t max_width);
bool glob_match(std::string_view text, std::string_view pattern);
bool file_path_matches_glob(std::string_view file_path, std::string_view pattern);
