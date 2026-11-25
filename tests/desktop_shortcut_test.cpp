#include "desktop_shortcut.h"
#include <windows.h>
#include <shobjidl.h>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace {
void check(bool ok, const std::string& why) {
    if (!ok) throw std::runtime_error(why);
}

class ShortcutFixture {
    IShellLinkW* link = nullptr;
    IPersistFile* file = nullptr;
public:
    ShortcutFixture() {
        check(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))),
              "Shell Link creation failed");
        check(SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file))), "IPersistFile unavailable");
    }
    ~ShortcutFixture() { if (file) file->Release(); if (link) link->Release(); }
    void save_old(const fs::path& path, const fs::path& target, const wchar_t* args = L"") {
        check(SUCCEEDED(link->SetPath(target.c_str())) && SUCCEEDED(link->SetWorkingDirectory(target.parent_path().c_str()))
              && SUCCEEDED(link->SetArguments(args)) && SUCCEEDED(link->SetIconLocation(target.c_str(), 0))
              && SUCCEEDED(file->Save(path.c_str(), TRUE)), "Could not create legacy test shortcut");
    }
    void verify_game(const fs::path& path, const fs::path& game, const fs::path& cwd) {
        wchar_t target[2048]{}, work[2048]{}, icon[2048]{}, args[2048]{};
        int index = -1;
        check(SUCCEEDED(file->Load(path.c_str(), STGM_READ)) && SUCCEEDED(link->GetPath(target,2048,nullptr,SLGP_RAWPATH))
              && SUCCEEDED(link->GetWorkingDirectory(work,2048)) && SUCCEEDED(link->GetIconLocation(icon,2048,&index))
              && SUCCEEDED(link->GetArguments(args,2048)), "Could not read shortcut");
        check(fs::equivalent(target,game) && fs::equivalent(work,cwd) && fs::equivalent(icon,game) && index==0 && !args[0],
              "Shortcut does not start the game directly with its working directory and icon");
    }
};

std::string bytes(const fs::path& path) {
    std::ifstream f(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(f),{}};
}
}

int main() try {
    check(SUCCEEDED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)), "Cannot initialize COM");
    const auto root=fs::temp_directory_path()/("ssne-shortcut-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    check(fs::create_directory(root),"Test directory unavailable");
    const auto install=root/L"Spiel mit Leerzeichen", desktop=root/L"Desktop";
    const auto game=install/L"SSNE-Native-Game.exe", legacy=install/L"SSNE-RECOOP-Native.exe";
    const auto cwd=install/L"Multiplayer", path=desktop/L"SSNE-RECOOP-Native.lnk", other=root/L"another.exe";
    fs::create_directories(cwd);fs::create_directory(desktop);
    for(const auto& exe:{game,legacy,other})std::ofstream(exe).put('x');
    std::string error;
    auto write=[&]{return launcher::write_game_shortcut(game,cwd,legacy,desktop,error);};
    check(write(),error); ShortcutFixture{}.verify_game(path,game,cwd);
    const auto stamp=fs::last_write_time(path);
    check(write() && fs::last_write_time(path)==stamp,"Identical game link was rewritten");
    ShortcutFixture{}.save_old(path,legacy);
    check(write(),error); ShortcutFixture{}.verify_game(path,game,cwd);
    for(const auto& target:{other,legacy}) {
        ShortcutFixture{}.save_old(path,target,target==legacy?L"--user-option":L"");
        const auto saved=bytes(path);
        check(!write() && !error.empty() && bytes(path)==saved,"Unrelated or customized shortcut was replaced");
    }
    fs::remove(path);fs::remove(game);
    check(!write() && !fs::exists(path),"Missing game produced a shortcut");
    fs::remove_all(root);CoUninitialize();
    std::cout<<"Windows game shortcut target, working directory, icon, idempotence, migration and collision checks passed.\n";
    return 0;
} catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
