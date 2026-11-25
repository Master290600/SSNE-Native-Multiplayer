#ifdef _WIN32
#include <windows.h>

// Built separately when older launchers need a compatibility package to reach
// a new runtime-kit protocol. The release packer pins the nested full archive.
#ifndef SSNE_BRIDGE_ARCHIVE
#error SSNE_BRIDGE_ARCHIVE must name the nested release archive
#endif
#ifndef SSNE_BRIDGE_SHA256
#error SSNE_BRIDGE_SHA256 must pin the nested release archive
#endif

#include "gc/zip.h"
#include "platform.h"
#include "runtime_kit.h"
#include "updater.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
constexpr char launcher_name[] = "SSNE-RECOOP-Native.exe";
constexpr char archive_name[] = SSNE_BRIDGE_ARCHIVE;
constexpr char archive_sha256[] = SSNE_BRIDGE_SHA256;

std::string utf8(const fs::path& path) {
    const auto bytes = path.u8string();
    return {bytes.begin(), bytes.end()};
}

void reject_reparse_point(const fs::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("Update path is a link: " + utf8(path));
}

void move_into_place(const fs::path& from, const fs::path& to) {
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot install " + utf8(to) + " (Windows error " + std::to_string(GetLastError()) + ")");
}

struct ScopedHandle {
    HANDLE value;
    void close() {
        if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
        value = INVALID_HANDLE_VALUE;
    }
    ~ScopedHandle() { close(); }
};

fs::path from_utf8(const std::string& text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

fs::path long_path(fs::path path) {
    const auto name = path.make_preferred().native();
    if (name.starts_with(LR"(\\?\)")) return path;
    if (name.starts_with(LR"(\\)")) return LR"(\\?\UNC\)" + name.substr(2);
    return LR"(\\?\)" + name;
}

int run_update() {
    const fs::path base = fs::canonical(from_utf8(platform::exe_dir()));
    const auto file_base = long_path(base);
    const fs::path pending = file_base / "release-payload";
    const auto archive = pending / archive_name;
    const auto update_root = base / "update";
    const auto validation_dir = update_root / "v";
    const auto staged = long_path(validation_dir);
    const auto existing = file_base / launcher_name;
    reject_reparse_point(pending);
    reject_reparse_point(archive);
    reject_reparse_point(long_path(update_root));
    reject_reparse_point(staged);
    reject_reparse_point(existing);

    // The source archive stays in place until all verified files have been installed.
    const auto lock_path = pending / "install.lock";
    reject_reparse_point(lock_path);
    ScopedHandle lock{CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  OPEN_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr)};
    if (lock.value == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_SHARING_VIOLATION) return 0;
        throw std::runtime_error("Cannot open the pending update. Keep the release-payload folder next to the launcher.");
    }

    std::string error;
    if (launcher::runtime_file_sha256(archive, error) != archive_sha256)
        throw std::runtime_error("The downloaded update payload failed its SHA-256 check. " + error);
    fs::remove_all(staged);
    fs::create_directories(staged);
    if (!gc::zip::extract(utf8(archive), utf8(staged), [](std::string name) {
                              std::replace(name.begin(), name.end(), '/', '\\');
                              return name;
                          },
                          [](uint64_t, uint64_t) { return true; }, error) ||
        !updater::validate_package(utf8(base), utf8(validation_dir), error))
        throw std::runtime_error("Cannot verify the update: " + error);

    std::vector<fs::path> files;
    for (const auto& item : fs::recursive_directory_iterator(staged)) {
        const auto relative = item.path().lexically_relative(staged);
        auto target = file_base;
        for (const auto& part : relative) {
            target /= part;
            reject_reparse_point(target);
        }
        if (item.is_regular_file() && relative != launcher_name) files.push_back(relative);
    }
    // A retry can extract the pinned archive again. Replace the retry-capable launcher last.
    std::sort(files.begin(), files.end());
    for (const auto& relative : files) {
        fs::create_directories((file_base / relative).parent_path());
        move_into_place(staged / relative, file_base / relative);
    }

    const auto running = file_base / "SSNE-RECOOP-Native.bridge.exe.old";
    const auto leftovers_path = base / "update-leftovers.txt";
    reject_reparse_point(running);
    reject_reparse_point(leftovers_path);
    std::ofstream leftovers(leftovers_path, std::ios::app);
    if (leftovers) leftovers << utf8(base / running.filename()) << '\n';
    leftovers.close();

    move_into_place(existing, running);
    try {
        move_into_place(staged / launcher_name, existing);
    } catch (...) {
        move_into_place(running, existing);
        throw;
    }

    std::error_code ignored;
    fs::remove_all(staged, ignored);
    fs::remove(archive, ignored);
    lock.close();
    fs::remove(pending, ignored);
    if (!platform::launch(utf8(base / launcher_name), {"--updated"}, utf8(base), error))
        throw std::runtime_error("The update was installed. Start SSNE-RECOOP-Native.exe again. " + error);
    return 0;
}
}  // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    try {
        return run_update();
    } catch (const std::exception& error) {
        fprintf(stderr, "SSNE-RECOOP-Native update: %s\n", error.what());
        OutputDebugStringA(error.what());
        if (platform::environment_utf8("SSNE_TEST_HIDDEN") != "1") {
            const std::string message = std::string(error.what()) +
                "\n\nClose any other launcher instance and open SSNE-RECOOP-Native.exe to retry.";
            MessageBoxW(nullptr, from_utf8(message).c_str(), L"SSNE-RECOOP-Native Update", MB_OK | MB_ICONERROR);
        }
        return 1;
    }
}
#endif
