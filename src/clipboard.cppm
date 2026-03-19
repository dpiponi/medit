module;

#include <filesystem>
#include <optional>
#include <string>

export module clipboard;

export import editor_core;

export struct ClipboardSnapshot {
    std::u32string text;
    SelectionMode mode = SelectionMode::Character;
};

export enum class ClipboardMode {
    Auto,
    Native,
    SharedFile,
    Internal,
};

export struct ClipboardConfig {
    ClipboardMode mode = ClipboardMode::Auto;
    std::filesystem::path shared_file_path;
    bool osc52 = true;
};

export class ClipboardProvider {
  public:
    explicit ClipboardProvider(ClipboardConfig config);

    const ClipboardConfig &config() const;
    ClipboardSnapshot read(const ClipboardSnapshot &fallback) const;
    bool write(const ClipboardSnapshot &snapshot) const;

  private:
    ClipboardConfig config_;
};

export ClipboardConfig default_clipboard_config();
