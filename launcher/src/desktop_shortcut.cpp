#include "desktop_shortcut.h"
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <string_view>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
namespace launcher {
static bool shell_result(HRESULT result, std::string& error) {
    if (SUCCEEDED(result)) return true;
    const int code = HRESULT_FACILITY(result) == FACILITY_WIN32 ? HRESULT_CODE(result) : static_cast<int>(result);
    if (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS)
        error = "A shortcut with this name already exists for another installation.";
    else
        error = "Windows error " + std::to_string(code) + ".";
    return false;
}

static constexpr wchar_t game_description[] = L"Play Serious Sam: Next Encounter (SSNE-RECOOP-Native)";

static bool same_location(const fs::path& a, const fs::path& b) {
    if (a.empty() || b.empty()) return false;
    std::error_code ec;
    return fs::equivalent(a, b, ec);
}

bool write_game_shortcut(const fs::path& game, const fs::path& working_dir, const fs::path& launcher_exe,
                         const fs::path& desktop, std::string& error) {
    error.clear();
    std::error_code ec;
    if (!fs::is_regular_file(game, ec) || !fs::is_directory(working_dir, ec)) {
        error = "Build the game before creating its desktop shortcut.";
        return false;
    }
    const fs::path path = desktop / L"SSNE-RECOOP-Native.lnk";
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (apartment != RPC_E_CHANGED_MODE && !shell_result(apartment, error)) return false;
    IShellLinkW* link = nullptr;
    IPersistFile* file = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
    if (SUCCEEDED(hr)) hr = link->QueryInterface(IID_PPV_ARGS(&file));
    const bool exists = fs::exists(path, ec);
    bool write = !exists;
    if (ec) hr = HRESULT_FROM_WIN32(ec.value());
    if (SUCCEEDED(hr) && exists) {
        hr = file->Load(path.c_str(), STGM_READ);
        constexpr int capacity = 32768;
        std::vector<wchar_t> current(capacity), cwd(capacity), icon(capacity), args(capacity), description(capacity);
        int icon_index = 0;
        WORD hotkey = 0;
        int show = SW_SHOWNORMAL;
        if (SUCCEEDED(hr)) hr = link->GetPath(current.data(), capacity, nullptr, SLGP_RAWPATH);
        if (SUCCEEDED(hr)) hr = link->GetWorkingDirectory(cwd.data(), capacity);
        if (SUCCEEDED(hr)) hr = link->GetIconLocation(icon.data(), capacity, &icon_index);
        if (SUCCEEDED(hr)) hr = link->GetArguments(args.data(), capacity);
        if (SUCCEEDED(hr)) hr = link->GetDescription(description.data(), capacity);
        if (SUCCEEDED(hr)) hr = link->GetHotkey(&hotkey);
        if (SUCCEEDED(hr)) hr = link->GetShowCmd(&show);
        if (SUCCEEDED(hr)) {
            const bool plain = !args[0] && icon_index == 0 && same_location(current.data(), icon.data());
            const bool correct = plain && same_location(current.data(), game) && same_location(cwd.data(), working_dir);
            // Older launchers made unmarked links with these exact target, icon, arguments and working directory.
            const bool old_launcher = plain && !description[0] && !hotkey && show == SW_SHOWNORMAL
                && same_location(current.data(), launcher_exe)
                && same_location(cwd.data(), launcher_exe.parent_path());
            const bool older_game = plain && std::wstring_view(description.data()) == game_description
                && same_location(cwd.data(), working_dir)
                && same_location(fs::path(current.data()).parent_path(), working_dir);
            if (!correct && !old_launcher && !older_game) hr = HRESULT_FROM_WIN32(ERROR_FILE_EXISTS);
            write = !correct;
        }
    }
    if (SUCCEEDED(hr) && write) {
        hr = link->SetPath(game.c_str());
        if (SUCCEEDED(hr)) hr = link->SetWorkingDirectory(working_dir.c_str());
        if (SUCCEEDED(hr)) hr = link->SetIconLocation(game.c_str(), 0);
        if (SUCCEEDED(hr)) hr = link->SetDescription(game_description);
        GUID id{};
        wchar_t guid[39]{};
        if (SUCCEEDED(hr)) hr = CoCreateGuid(&id);
        if (SUCCEEDED(hr) && !StringFromGUID2(id, guid, 39)) hr = E_FAIL;
        if (SUCCEEDED(hr)) {
            const fs::path staging = desktop / (std::wstring(L".ssne-") + guid + L".lnk");
            hr = file->Save(staging.c_str(), FALSE);
            const DWORD flags = MOVEFILE_WRITE_THROUGH | (exists ? MOVEFILE_REPLACE_EXISTING : 0);
            if (SUCCEEDED(hr) && !MoveFileExW(staging.c_str(), path.c_str(), flags))
                hr = HRESULT_FROM_WIN32(GetLastError());
            if (FAILED(hr)) fs::remove(staging, ec);
        }
    }
    if (file) file->Release();
    if (link) link->Release();
    if (SUCCEEDED(apartment)) CoUninitialize();
    return shell_result(hr, error);
}

bool create_desktop_shortcut(const fs::path& game, const fs::path& working_dir, std::string& error) {
    std::vector<wchar_t> exe(32768);
    const DWORD size = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    if (!size || size == exe.size()) return shell_result(HRESULT_FROM_WIN32(size ? ERROR_INSUFFICIENT_BUFFER : GetLastError()), error);
    PWSTR desktop = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop);
    if (!shell_result(hr, error)) return false;
    const fs::path folder(desktop);
    CoTaskMemFree(desktop);
    return write_game_shortcut(game, working_dir, fs::path(exe.data()), folder, error);
}
}
#endif
