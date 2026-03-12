#pragma once

#include "editor_core.hpp"

#include <filesystem>
#include <optional>
#include <string>

struct ClipboardSnapshot {
    std::u32string text;
    SelectionMode mode = SelectionMode::Character;
};

enum class ClipboardMode {
    Auto,
    Native,
    SharedFile,
    Internal,
};

struct ClipboardConfig {
    ClipboardMode mode = ClipboardMode::Auto;
    std::filesystem::path shared_file_path;
    bool osc52 = true;
};

class ClipboardProvider {
  public:
    explicit ClipboardProvider(ClipboardConfig config);

    const ClipboardConfig &config() const;
    ClipboardSnapshot read(const ClipboardSnapshot &fallback) const;
    bool write(const ClipboardSnapshot &snapshot) const;

  private:
    ClipboardConfig config_;
};

ClipboardConfig default_clipboard_config();
