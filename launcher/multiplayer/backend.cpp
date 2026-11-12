#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>
#include <shlobj.h>
#include <algorithm>
#include <charconv>
#include <cctype>
#include <chrono>
#include <limits>
#include "backend.h"
namespace multiplayer_launcher {
std::wstring wide(const std::string& text){if(text.empty())return {};int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),nullptr,0);std::wstring out(n,L'\0');if(n)MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),out.data(),n);return out;}
std::string utf8(const std::wstring& text){if(text.empty())return {};int n=WideCharToMultiByte(CP_UTF8,0,text.data(),int(text.size()),nullptr,0,nullptr,nullptr);std::string out(n,'\0');if(n)WideCharToMultiByte(CP_UTF8,0,text.data(),int(text.size()),out.data(),n,nullptr,nullptr);return out;}
std::filesystem::path exe_directory(){std::wstring path(32768,L'\0');auto n=GetModuleFileNameW(nullptr,path.data(),DWORD(path.size()));path.resize(n);return std::filesystem::path(path).parent_path();}
std::wstring quote_argument(const std::wstring& text){
 std::wstring out=L"\"";unsigned slashes=0;
 for(auto c:text){if(c==L'\\'){++slashes;continue;}if(c==L'"'){out.append(slashes*2+1,L'\\');out+=c;}else{out.append(slashes,L'\\');out+=c;}slashes=0;}
 out.append(slashes*2,L'\\');out+=L'"';return out;
}
namespace {
size_t field(const std::string& json,const char* key){auto p=json.find(std::string("\"")+key+"\"");if(p==std::string::npos)return p;p=json.find(':',p+strlen(key)+2);if(p==std::string::npos)return p;do{++p;}while(p<json.size() && std::isspace((unsigned char)json[p]));return p;}
std::string string_field(const std::string& json,const char* key){
 auto p=field(json,key);if(p==std::string::npos || p>=json.size() || json[p]!='"')return {};std::string out;
 for(++p;p<json.size();++p){char c=json[p];if(c=='"')return out;if(c!='\\'){out+=c;continue;}if(++p==json.size())return {};
  switch(json[p]){case '"':out+='"';break;case '\\':out+='\\';break;case '/':out+='/';break;case 'n':out+='\n';break;case 'r':out+='\r';break;case 't':out+='\t';break;case 'b':out+='\b';break;case 'f':out+='\f';break;
   case 'u':{if(p+4>=json.size())return {};unsigned value=0;for(int i=0;i<4;++i){char h=json[++p];value<<=4;if(h>='0'&&h<='9')value+=h-'0';else if(h>='a'&&h<='f')value+=h-'a'+10;else if(h>='A'&&h<='F')value+=h-'A'+10;else return {};}
    std::wstring chars(1,wchar_t(value));if(value>=0xD800 && value<=0xDBFF && p+6<json.size() && json[p+1]=='\\' && json[p+2]=='u'){unsigned low=0;auto result=std::from_chars(json.data()+p+3,json.data()+p+7,low,16);if(result.ec!=std::errc{} || low<0xDC00 || low>0xDFFF)return {};chars+=wchar_t(low);p+=6;}out+=utf8(chars);break;}
   default:return {};
  }
 }
 return {};
}
int integer_field(const std::string& json,const char* key){auto p=field(json,key);if(p==std::string::npos)return 0;int value=0;auto r=std::from_chars(json.data()+p,json.data()+json.size(),value);return r.ec==std::errc{} && value>=0?value:0;}
bool boolean_field(const std::string& json,const char* key){auto p=field(json,key);return p!=std::string::npos && json.compare(p,4,"true")==0 && (p+4==json.size() || json[p+4]==',' || json[p+4]=='}' || std::isspace((unsigned char)json[p+4]));}
std::vector<wchar_t> child_environment(){
 std::vector<wchar_t> out;auto env=GetEnvironmentStringsW();if(!env)return out;
 for(auto p=env;*p;p+=wcslen(p)+1){std::wstring entry=p,key=entry.substr(0,entry.find(L'='));
  // A launcher must never inherit automated game inputs or an incompatible
  // PowerShell7 module path into the Windows PowerShell helper.
  if(_wcsnicmp(key.c_str(),L"SSNE_",5)==0 || _wcsicmp(key.c_str(),L"PSModulePath")==0)continue;
  out.insert(out.end(),entry.begin(),entry.end());out.push_back(0);
 }
 FreeEnvironmentStringsW(env);out.push_back(0);if(out.size()==1)out.push_back(0);return out;
}
struct Handle {HANDLE h=nullptr;~Handle(){if(h && h!=INVALID_HANDLE_VALUE)CloseHandle(h);}operator HANDLE()const{return h;}};
}
Result parse_result(const std::string& json){
 Result out;out.message=string_field(json,"message");out.ok=boolean_field(json,"ok");out.ready=boolean_field(json,"ready");out.build=integer_field(json,"build");out.protocol=integer_field(json,"protocol");out.pid=integer_field(json,"pid");out.project=string_field(json,"project");out.exe=string_field(json,"exe");
 if(out.message.empty()){out.ok=false;out.ready=false;out.message="Der Starter hat keine gültige Antwort geliefert. Bitte den Multiplayer-Ordner prüfen.";}
 if(out.ok && (!out.build || !out.protocol || out.project.empty() || out.exe.empty())){out.ok=false;out.ready=false;out.message="Die Build-Information ist unvollständig. Bitte den Multiplayer-Ordner prüfen.";}
 return out;
}
Result run_backend(const std::filesystem::path& directory,const Request& request){
 Result result;std::error_code ec;const auto script=directory/L"Start-Multiplayer-GUI.ps1";
 if(!std::filesystem::is_regular_file(script,ec)){result.message="Die Starter-Datei fehlt neben dem Launcher.";return result;}
 wchar_t windows[MAX_PATH];GetWindowsDirectoryW(windows,MAX_PATH);auto shell=std::filesystem::path(windows)/L"System32/WindowsPowerShell/v1.0/powershell.exe";
 std::vector<std::wstring> args={shell.wstring(),L"-NoLogo",L"-NoProfile",L"-NonInteractive",L"-ExecutionPolicy",L"Bypass",L"-File",script.wstring(),L"-Mode",wide(request.mode),L"-Port",std::to_wstring(request.port)};
 if(!request.project.empty()){args.push_back(L"-Project");args.push_back(wide(request.project));}
 if(!request.address.empty()){args.push_back(L"-Address");args.push_back(wide(request.address));}
 if(!request.pin.empty()){args.push_back(L"-Pin");args.push_back(wide(request.pin));}
 args.push_back(L"-PlayerName");args.push_back(wide(request.player_name));
 args.push_back(L"-PlayerSkin");args.push_back(std::to_wstring(request.player_skin));
 args.push_back(L"-EnemyMultiplier");args.push_back(std::to_wstring(request.enemy_multiplier));
 args.push_back(L"-AmmoCapacity");args.push_back(std::to_wstring(request.ammo_capacity));
 args.push_back(L"-AmmoPickupMultiplier");args.push_back(std::to_wstring(request.ammo_pickup_multiplier));
 if(request.describe)args.push_back(L"-Describe");if(request.validate_only)args.push_back(L"-ValidateOnly");if(request.fullscreen)args.push_back(L"-Fullscreen");
 std::wstring command;for(const auto& arg:args){if(!command.empty())command+=' ';command+=quote_argument(arg);}
 SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};Handle read,write;HANDLE a,b;if(!CreatePipe(&a,&b,&sa,0)){result.message="Der Starter konnte nicht vorbereitet werden.";return result;}read.h=a;write.h=b;SetHandleInformation(read,HANDLE_FLAG_INHERIT,0);
 Handle input;input.h=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,nullptr);
 // Inherit only the helper's streams, never unrelated handles from the launcher.
 STARTUPINFOEXW si{};si.StartupInfo.cb=sizeof(si);si.StartupInfo.dwFlags=STARTF_USESTDHANDLES;si.StartupInfo.hStdOutput=write;si.StartupInfo.hStdError=write;si.StartupInfo.hStdInput=input;
 SIZE_T attribute_bytes=0;InitializeProcThreadAttributeList(nullptr,1,0,&attribute_bytes);std::vector<unsigned char> attributes(attribute_bytes);
 si.lpAttributeList=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
 if(!InitializeProcThreadAttributeList(si.lpAttributeList,1,0,&attribute_bytes)){result.message="Der Starter konnte nicht vorbereitet werden.";return result;}
 HANDLE inherited[]={write.h,input.h};
 if(!UpdateProcThreadAttribute(si.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),nullptr,nullptr)){DeleteProcThreadAttributeList(si.lpAttributeList);result.message="Der Starter konnte nicht vorbereitet werden.";return result;}
 PROCESS_INFORMATION pi{};auto environment=child_environment();
 const auto started=CreateProcessW(shell.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT|EXTENDED_STARTUPINFO_PRESENT,environment.data(),directory.c_str(),&si.StartupInfo,&pi);const auto start_error=GetLastError();DeleteProcThreadAttributeList(si.lpAttributeList);
 if(!started){result.message="Der Windows-Starter konnte nicht geöffnet werden ("+std::to_string(start_error)+").";return result;}
 Handle process,thread;process.h=pi.hProcess;thread.h=pi.hThread;CloseHandle(write.h);write.h=nullptr;
 std::string output;const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(45);
 for(;;){
  DWORD available=0;if(PeekNamedPipe(read,nullptr,0,nullptr,&available,nullptr) && available){char buffer[4096];DWORD count=0;if(ReadFile(read,buffer,std::min<DWORD>(available,sizeof(buffer)),&count,nullptr) && output.size()<65536)output.append(buffer,std::min<size_t>(count,65536-output.size()));continue;}
  if(WaitForSingleObject(process,20)==WAIT_OBJECT_0){DWORD tail=0;if(PeekNamedPipe(read,nullptr,0,nullptr,&tail,nullptr) && tail)continue;break;}
  if(std::chrono::steady_clock::now()>deadline){TerminateProcess(process,1);result.message="Der Starter antwortet nicht. Bitte erneut versuchen.";return result;}
 }
 auto start=output.find('{'),end=output.rfind('}');if(start!=std::string::npos && end!=std::string::npos && end>=start)result=parse_result(output.substr(start,end-start+1));else result=parse_result("");
 DWORD code=1;GetExitCodeProcess(process,&code);if(code && result.ok){result.ok=false;result.ready=false;result.message="Das Spiel konnte nicht gestartet werden.";}return result;
}
std::string choose_project(){
 auto initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);IFileDialog* picker=nullptr;std::string result;
 if(SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&picker)))){
  DWORD flags=0;picker->GetOptions(&flags);picker->SetOptions(flags|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_NOCHANGEDIR);picker->SetTitle(L"Spielordner auswählen (enthält game_data)");
  if(SUCCEEDED(picker->Show(nullptr))){IShellItem* item=nullptr;if(SUCCEEDED(picker->GetResult(&item))){PWSTR path=nullptr;if(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&path))){result=utf8(path);CoTaskMemFree(path);}item->Release();}}
  picker->Release();
 }
 if(SUCCEEDED(initialized))CoUninitialize();return result;
}
std::string local_address(){
 ULONG bytes=16384;std::vector<unsigned char> buffer(bytes);auto* adapters=reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());ULONG code=GetAdaptersAddresses(AF_INET,GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER,nullptr,adapters,&bytes);
 if(code==ERROR_BUFFER_OVERFLOW){buffer.resize(bytes);adapters=reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());code=GetAdaptersAddresses(AF_INET,GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER,nullptr,adapters,&bytes);}if(code!=NO_ERROR)return "127.0.0.1";
 for(auto* a=adapters;a;a=a->Next)if(a->OperStatus==IfOperStatusUp && a->IfType!=IF_TYPE_SOFTWARE_LOOPBACK && a->IfType!=IF_TYPE_TUNNEL){for(auto* u=a->FirstUnicastAddress;u;u=u->Next){auto* address=reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);if(!address || address->sin_family!=AF_INET)continue;char text[64]{};if(InetNtopA(AF_INET,&address->sin_addr,text,sizeof(text)) && strncmp(text,"169.254.",8)!=0)return text;}}
 return "127.0.0.1";
}
}
