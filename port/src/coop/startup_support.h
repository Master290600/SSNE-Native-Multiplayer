#pragma once
#include <filesystem>
#include <string>

namespace port::startup {
inline std::string data_directory(const std::string& base, const std::string& explicit_path) {
    if (!explicit_path.empty()) return explicit_path;
    for (const auto& candidate : {std::filesystem::u8path(base)/"game_data",
                                  std::filesystem::u8path(base)/".."/"game_data"}) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate/"sys"/"main.dol",ec))
            return candidate.lexically_normal().string();
    }
    return (std::filesystem::u8path(base)/"game_data").string();
}
// UI rendering needs its own bound even when the game clock has not started,
// has stopped on an error, or is paused. Never depend only on driver vsync.
inline int presentation_limit(int requested, bool background, bool idle) {
    if (background || idle) return requested > 0 ? (requested < 30 ? requested : 30) : 30;
    return requested;
}
}
