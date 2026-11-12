#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace launcher {
struct RuntimeKitFile {
    std::string path,sha256;
};
struct RuntimeKit {
    std::string project_dir,kit_dir,data_dir;
    std::string descriptor_sha256,sidecar_sha256,fingerprint;
    std::string recipe,recipe_sha256,symbols,symbols_sha256;
    std::string compiler,compiler_sha256,recompiler,recompiler_sha256,dol_sha256;
    int build=0,protocol=0;
    std::vector<RuntimeKitFile> files;
    std::vector<std::string> compile_flags,link_args;
};

// Paths are UTF-8. PAL data and the sidecar/descriptor/tools/payload are pinned;
// the compiler may be absent until GameBuilder obtains the base toolchain.
// Updater package checks may set verify_disc=false before disc installation;
// build and launch keep the default true and always verify the owned PAL data.
bool load_runtime_kit(const std::string& project_dir,const std::string& data_dir,
                      RuntimeKit&,std::string& error,bool verify_disc=true);
bool verify_runtime_kit(const RuntimeKit&,bool require_compiler,std::string& error);
std::string runtime_file_sha256(const std::filesystem::path&,std::string& error);
std::string runtime_text_sha256(const std::string&);
// Accepts either a matching transactional local-build receipt or the shipped
// native release manifest. It always checks the executable and alias hashes.
bool runtime_build_ready(const RuntimeKit&,const std::string& output_exe,
                         std::string& error,bool force_verify=false);
}
