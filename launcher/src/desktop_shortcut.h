#pragma once
#include <filesystem>
#include <string>

namespace launcher {
#ifdef _WIN32
bool create_desktop_shortcut(const std::filesystem::path& game, const std::filesystem::path& working_dir,
                             std::string& error);
bool write_game_shortcut(const std::filesystem::path& game, const std::filesystem::path& working_dir,
                         const std::filesystem::path& launcher_exe, const std::filesystem::path& desktop,
                         std::string& error);
#endif
}
