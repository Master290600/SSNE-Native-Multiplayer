// Reuse the valid SSW/PNG fixture and assertion helpers of the package suite.
#define main custom_level_fixture_main
#include "custom_levels_test.cpp"
#undef main
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
launcher::JSONValue load_result(const fs::path& path) {
  const auto data=read(path);launcher::JSONValue value;std::string error;
  check(launcher::parse_json(std::string_view(reinterpret_cast<const char*>(data.data()),data.size()),value,error),"Read child-process QA result");
  return value;
}
void save_result(const fs::path& path,const launcher::JSONValue& value) {
  const auto text=launcher::write_json(value,true)+"\n";
  write(fs::absolute(path),{text.begin(),text.end()});
}
int64_t number(const launcher::JSONValue& value,const char* key) {
  const auto* field=value.find(key);int64_t n=0;
  check(field && field->integer(n),"Read numeric worker evidence");return n;
}
std::string checksum(const std::vector<uint8_t>& data) {
  gc::Sha256 hash;hash.update(data.data(),data.size());return gc::Sha256::hex(hash.finish());
}
int worker(const fs::path& root,const std::string& mode) {
  const auto source=gc::custom::scan(root/"OriginalCustomLevels");
  check(source.levels.size()==1,"Worker source package exists");
  const auto level=source.levels.front();const auto map=read(level.map_path),image=read(level.preview_path);
  const auto package_sha=gc::custom::catalog_fingerprint(source),preview_sha=checksum(image);
  const auto start=std::chrono::steady_clock::now();
  while(!fs::exists(root/"start.signal")) {
    check(std::chrono::steady_clock::now()-start<std::chrono::seconds(20),"Independent process start barrier");
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  int64_t blocked=0,observed=0,empty=0;
  const auto cache=root/"DownloadedCustomLevels";
  if(mode=="reader") {
    const auto reading_started=std::chrono::steady_clock::now();
    for(unsigned attempt=0;attempt<400 &&
        std::chrono::steady_clock::now()-reading_started<std::chrono::seconds(8);++attempt) {
#ifdef _WIN32
      // CacheLock uses the same exclusive OS file handle in each real process.
      const auto handle=CreateFileW((cache/".custom-levels.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
      if(handle==INVALID_HANDLE_VALUE) {
        const auto error=GetLastError();
        if(error==ERROR_SHARING_VIOLATION || error==ERROR_LOCK_VIOLATION)++blocked;
      } else CloseHandle(handle);
#endif
      if(auto hit=gc::custom::find_downloaded_package(cache,level.alias,level.id,level.map_sha256)) {
        check(hit->level.title==level.title && hit->package_sha256==package_sha &&
                  hit->preview_sha256==preview_sha && read(hit->level.map_path)==map && read(hit->level.preview_path)==image,
              "Reader only exposes complete matching map, name and PNG");
        ++observed;
      } else ++empty;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  } else {
    const auto first=gc::custom::write_downloaded_package(cache,level,map,image,package_sha,preview_sha);
    for(unsigned attempt=0;attempt<2;++attempt)
      check(gc::custom::write_downloaded_package(cache,level,map,image,package_sha,preview_sha).folder==first.folder,
            "Writer reuses the same complete immutable cache version");
  }
  save_result(root/(mode+"-result.json"),launcher::JSONValue::Object{
      {"schema",1},{"passed",true},{"pid",int64_t(GetCurrentProcessId())},
      {"blocked_lock_observations",blocked},{"installed_observations",observed},
      {"incomplete_observations",empty},{"package_sha256",package_sha}});
  return 0;
}
#ifdef _WIN32
struct Child { PROCESS_INFORMATION process{};std::string mode; };
Child spawn(const fs::path& root,const std::string& mode) {
  wchar_t executable[32768]{};
  check(GetModuleFileNameW(nullptr,executable,32768)>0,"Locate worker executable");
  const std::wstring wide_mode(mode.begin(),mode.end());
  auto command=L"\""+std::wstring(executable)+L"\" --worker \""+root.wstring()+L"\" "+wide_mode;
  STARTUPINFOW startup{};startup.cb=sizeof(startup);Child child;child.mode=mode;
  check(CreateProcessW(executable,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child.process)!=0,
        "Start a separate hidden Windows cache process");
  return child;
}
#endif
} // namespace
int main(int argc,char** argv) {
  try {
    if(argc==4 && std::string(argv[1])=="--worker")return worker(fs::u8path(argv[2]),argv[3]);
    check(argc==3,"Usage: concurrency_test original_game_data private_evidence_root");
    const auto root=fs::absolute(fs::u8path(argv[2]));
    check(!fs::exists(root),"Evidence root is new and isolated");
    fs::create_directories(root/"DownloadedCustomLevels");
    write(root/"preview-source.png",png());
    const auto original_data=fs::u8path(argv[1]);
    const auto original_fst=read(original_data/"sys/fst.bin"),original_boot=read(original_data/"sys/boot.bin");
    const auto map=read(original_data/"files/Levels/Rlevel1_1.ssw");
    const auto level=gc::custom::write_package(root/"OriginalCustomLevels","ConcurrentMap",
        "Concurrent host display name","Rlevel1_1",map,root/"preview-source.png");
    const auto original_map=read(level.map_path),original_preview=read(level.preview_path),original_metadata=read(level.folder/"level.json");
#ifdef _WIN32
    std::vector<Child> children;
    children.push_back(spawn(root,"writer0"));children.push_back(spawn(root,"writer1"));children.push_back(spawn(root,"reader"));
    write(root/"start.signal",{1});
    launcher::JSONValue::Array workers;
    int64_t blocked=0,observed=0;
    for(auto& child:children) {
      const auto waited=WaitForSingleObject(child.process.hProcess,120000);
      DWORD code=~DWORD(0);GetExitCodeProcess(child.process.hProcess,&code);
      CloseHandle(child.process.hThread);CloseHandle(child.process.hProcess);
      check(waited==WAIT_OBJECT_0 && code==0,"Separate cache worker completed successfully");
      auto result=load_result(root/(child.mode+"-result.json"));
      check(number(result,"pid")!=int64_t(GetCurrentProcessId()),"Worker PID differs from parent");
      if(child.mode=="reader") {blocked=number(result,"blocked_lock_observations");observed=number(result,"installed_observations");}
      workers.push_back(std::move(result));
    }
    check(blocked>0,"Reader observed exclusive OS lock held by independent cache writer");
    check(observed>0,"Concurrent reader saw at least one fully verified cache version");
    const auto installed=gc::custom::find_downloaded_package(root/"DownloadedCustomLevels",level.alias,level.id,level.map_sha256);
    check(installed && installed->level.title==level.title && read(installed->level.map_path)==map && read(installed->level.preview_path)==original_preview,
          "Both writers produced one matching map, custom name and PNG");
    size_t versions=0;
    for(const auto& entry:fs::directory_iterator(root/"DownloadedCustomLevels"))if(entry.is_directory())++versions;
    check(versions==1,"Concurrent same-package clients share one immutable completed cache version");
    check(read(level.map_path)==original_map && read(level.preview_path)==original_preview && read(level.folder/"level.json")==original_metadata,
          "Original local package bytes remain unchanged across all three processes");
    check(read(original_data/"sys/fst.bin")==original_fst && read(original_data/"sys/boot.bin")==original_boot &&
              read(original_data/"files/Levels/Rlevel1_1.ssw")==map,
          "Owned original disc FST, boot data and map remain unchanged");
    save_result("qa-cache-concurrency-result.json",launcher::JSONValue::Object{
        {"schema",1},{"passed",true},{"checks",int64_t(checks)},
        {"independent_processes",3},{"exclusive_lock_observations",blocked},
        {"verified_reader_observations",observed},{"cache_versions",int64_t(versions)},
        {"originals_unchanged",true},{"evidence_root",root.string()},
        {"map_sha256",checksum(original_map)},{"preview_sha256",checksum(original_preview)},
        {"workers",std::move(workers)}});
    std::cout<<"PASS independent_processes=3 checks="<<checks<<" lock_observations="<<blocked<<" verified_observations="<<observed<<"\n";
#else
    throw std::runtime_error("This regression verifies Windows OS sharing locks");
#endif
  } catch(const std::exception& e) {
    std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<"\n";return 1;
  }
}
