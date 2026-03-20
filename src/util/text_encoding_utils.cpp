#include "text_encoding_utils.hpp"

#include <stdexcept>

// Manual UTF-8 to UTF-32 conversion (C++23 compatible)
// wstring_convert was deprecated in C++17 and removed in C++23
std::u32string utf8_to_u32(const std::string &text) {
    std::u32string result;
    result.reserve(text.size()); // Reserve approximate size

    const unsigned char *bytes = reinterpret_cast<const unsigned char*>(text.data());
    size_t i = 0;

    while (i < text.size()) {
        char32_t codepoint = 0;
        size_t bytes_to_read = 0;

        unsigned char first = bytes[i];

        if ((first & 0x80) == 0) {
            // 1-byte sequence (0xxxxxxx)
            codepoint = first;
            bytes_to_read = 1;
        } else if ((first & 0xE0) == 0xC0) {
            // 2-byte sequence (110xxxxx 10xxxxxx)
            codepoint = first & 0x1F;
            bytes_to_read = 2;
        } else if ((first & 0xF0) == 0xE0) {
            // 3-byte sequence (1110xxxx 10xxxxxx 10xxxxxx)
            codepoint = first & 0x0F;
            bytes_to_read = 3;
        } else if ((first & 0xF8) == 0xF0) {
            // 4-byte sequence (11110xxx 10xxxxxx 10xxxxxx 10xxxxxx)
            codepoint = first & 0x07;
            bytes_to_read = 4;
        } else {
            // Invalid UTF-8 sequence - replace with replacement character
            result.push_back(0xFFFD);
            i++;
            continue;
        }

        // Check if we have enough bytes
        if (i + bytes_to_read > text.size()) {
            // Incomplete sequence - replace with replacement character
            result.push_back(0xFFFD);
            break;
        }

        // Read continuation bytes
        for (size_t j = 1; j < bytes_to_read; j++) {
            unsigned char byte = bytes[i + j];
            if ((byte & 0xC0) != 0x80) {
                // Invalid continuation byte
                codepoint = 0xFFFD;
                break;
            }
            codepoint = (codepoint << 6) | (byte & 0x3F);
        }

        result.push_back(codepoint);
        i += bytes_to_read;
    }

    return result;
}

std::string u32_to_utf8(const std::u32string &text) {
    std::string result;
    result.reserve(text.size() * 3); // Reserve approximate size (most chars are 1-3 bytes)

    for (char32_t codepoint : text) {
        if (codepoint < 0x80) {
            // 1-byte sequence
            result.push_back(static_cast<char>(codepoint));
        } else if (codepoint < 0x800) {
            // 2-byte sequence
            result.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
            result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else if (codepoint < 0x10000) {
            // 3-byte sequence
            result.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
            result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else if (codepoint < 0x110000) {
            // 4-byte sequence
            result.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
            result.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else {
            // Invalid codepoint - use replacement character
            result.push_back(static_cast<char>(0xEF));
            result.push_back(static_cast<char>(0xBF));
            result.push_back(static_cast<char>(0xBD));
        }
    }

    return result;
}
