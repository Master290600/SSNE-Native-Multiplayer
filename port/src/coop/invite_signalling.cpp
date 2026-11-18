#include "invite_signalling.h"
#include "../../../launcher/src/launcher_json.h"
#include <windows.h>
#include <winhttp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
namespace port::coop::invite { namespace {
constexpr size_t max_publish=4096,max_message=16384,max_sdp=8192,max_stream_line=32768;
using Clock=std::chrono::steady_clock;
bool hex(const std::string& text,size_t count){return text.size()==count && std::all_of(text.begin(),text.end(),[](unsigned char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F');});}
std::string lower(std::string text){for(char& c:text)if(c>='A'&&c<='Z')c+=32;return text;}
bool signal_valid(const Signal& s){
    if(!hex(s.sender,32) || (!s.target.empty()&&!hex(s.target,32)) || (s.target.empty() && s.kind!=Signal::Offer && !(s.kind==Signal::Cancel && s.description.empty())) || s.description.size()>max_sdp)return false;
    if(s.kind!=Signal::Offer && s.kind!=Signal::Answer && s.kind!=Signal::Cancel)return false;
    if(s.kind!=Signal::Cancel && s.description.empty())return false;
    return std::none_of(s.description.begin(),s.description.end(),[](unsigned char c){return c<32 && c!='\r' && c!='\n' && c!='\t';});
}
std::string fingerprint_text(uint64_t value){constexpr char chars[]="0123456789abcdef";std::string out(16,'0');for(int i=15;i>=0;--i){out[size_t(i)]=chars[value&15];value>>=4;}return out;}
std::string encode_signal(const Signal& s){
    return launcher::write_json(launcher::JSONValue::Object{{"kind",s.kind==Signal::Offer?"offer":s.kind==Signal::Answer?"answer":"cancel"},{"sender",lower(s.sender)},{"target",lower(s.target)},{"description",s.description},{"fingerprint",fingerprint_text(s.fingerprint)}});
}
bool decode_signal(const std::string& text,Signal& result){
    if(text.size()>max_message)return false;launcher::JSONValue json;std::string error;
    if(!launcher::parse_json(text,json,error,{max_message,max_sdp+256,32,4}) || !json.object() || json.object()->size()!=5)return false;
    auto string=[&](const char* key)->const std::string*{auto value=json.find(key);return value?value->string():nullptr;};
    const auto kind=string("kind"),sender=string("sender"),target=string("target"),description=string("description"),fingerprint=string("fingerprint");
    if(!kind||!sender||!target||!description||!fingerprint||!hex(*fingerprint,16))return false;
    Signal s;if(*kind=="offer")s.kind=Signal::Offer;else if(*kind=="answer")s.kind=Signal::Answer;else if(*kind=="cancel")s.kind=Signal::Cancel;else return false;
    s.sender=lower(*sender);s.target=lower(*target);s.description=*description;
    for(unsigned char c:lower(*fingerprint))s.fingerprint=(s.fingerprint<<4)|uint64_t(c<='9'?c-'0':c-'a'+10);
    if(!signal_valid(s))return false;result=std::move(s);return true;
}
std::wstring widen(const std::string& text){
    if(text.empty())return {};const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),nullptr,0);if(count<=0)return {};
    std::wstring out(size_t(count),L'\0');if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),out.data(),count))return {};return out;
}
struct Endpoint {std::wstring host,path;INTERNET_PORT port=0;bool secure=false,local=false;};
bool endpoint(const std::string& url,const std::string& topic,Endpoint& out,std::string& error){
    if(url.size()>2048 || topic.size()!=28 || topic.compare(0,8,"ssne-v1-") || !hex(topic.substr(8),20)){error="Invalid signalling server or invitation topic.";return false;}
    auto wide=widen(url);if(wide.empty()){error="Invalid signalling URL.";return false;}
    URL_COMPONENTS u{};u.dwStructSize=sizeof(u);u.dwHostNameLength=u.dwUrlPathLength=u.dwExtraInfoLength=u.dwUserNameLength=u.dwPasswordLength=DWORD(-1);
    if(!WinHttpCrackUrl(wide.c_str(),DWORD(wide.size()),0,&u) || !u.dwHostNameLength || u.dwHostNameLength>253 || u.dwUrlPathLength>1024 || u.dwExtraInfoLength || u.dwUserNameLength || u.dwPasswordLength || (u.nScheme!=INTERNET_SCHEME_HTTP && u.nScheme!=INTERNET_SCHEME_HTTPS)){error="Use an HTTPS signalling base URL without credentials or query.";return false;}
    out.host.assign(u.lpszHostName,u.dwHostNameLength);auto host=out.host;for(auto& c:host)if(c>=L'A'&&c<=L'Z')c+=32;
    out.local=host==L"127.0.0.1" || host==L"localhost";out.secure=u.nScheme==INTERNET_SCHEME_HTTPS;
    if(!out.secure&&!out.local){error="Signalling requires HTTPS; plain HTTP is restricted to loopback tests.";return false;}
    if(!u.nPort){error="Invalid signalling server port.";return false;}
    out.port=u.nPort;out.path.assign(u.lpszUrlPath,u.dwUrlPathLength);while(!out.path.empty()&&out.path.back()==L'/')out.path.pop_back();out.path+=L"/"+widen(topic);return true;
}
// Only the owner calls WinHTTP functions on a request. Other threads cancel by
// waking its wait; the owner then closes the asynchronous request and retains
// callback state until HANDLE_CLOSING, including cancellation during start/POST.
class HttpRequest {
    HINTERNET session_=nullptr,connection_=nullptr,request_=nullptr;
    std::mutex mutex_;std::condition_variable changed_;DWORD completed_=0,error_=0,read_size_=0,available_=0;bool cancelled_=false,closed_=false,callback_=false;
    static void CALLBACK status(HINTERNET,DWORD_PTR context,DWORD status,void* info,DWORD size){
        if(!context)return;auto& self=*reinterpret_cast<HttpRequest*>(context);std::lock_guard lock(self.mutex_);
        if(status==WINHTTP_CALLBACK_STATUS_REQUEST_ERROR && info && size>=sizeof(WINHTTP_ASYNC_RESULT))self.error_=static_cast<WINHTTP_ASYNC_RESULT*>(info)->dwError;
        if(status==WINHTTP_CALLBACK_STATUS_READ_COMPLETE)self.read_size_=size;
        if(status==WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE && info && size>=sizeof(DWORD))self.available_=*static_cast<DWORD*>(info);
        if(status==WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING)self.closed_=true;
        self.completed_|=status;self.changed_.notify_all();
    }
    void reset(){std::lock_guard lock(mutex_);completed_=error_=read_size_=available_=0;}
    bool wait(DWORD expected,std::chrono::milliseconds timeout,std::string& error){
        std::unique_lock lock(mutex_);if(!changed_.wait_for(lock,timeout,[&]{return cancelled_||error_||(completed_&expected); })){error="Signalling request timed out.";return false;}
        if(cancelled_){error="Signalling cancelled.";return false;}if(error_){error="Signalling request failed ("+std::to_string(error_)+").";return false;}return true;
    }
    bool accepted(BOOL ok,std::string& error){if(ok || GetLastError()==ERROR_IO_PENDING)return true;error="Signalling HTTP operation failed ("+std::to_string(GetLastError())+").";return false;}
    bool cancelled(){std::lock_guard lock(mutex_);return cancelled_;}
public:
    ~HttpRequest(){close();}
    void close(){
        if(request_){auto handle=request_;request_=nullptr;WinHttpCloseHandle(handle);if(callback_){std::unique_lock lock(mutex_);changed_.wait(lock,[&]{return closed_;});}}
        if(connection_){WinHttpCloseHandle(connection_);connection_=nullptr;}if(session_){WinHttpCloseHandle(session_);session_=nullptr;}
    }
    void cancel(){std::lock_guard lock(mutex_);cancelled_=true;changed_.notify_all();}
    bool open(const Endpoint& ep,const wchar_t* method,bool stream,std::string& error){
        if(cancelled()){error="Signalling cancelled.";return false;}
        session_=WinHttpOpen(L"SSNE-Native-Invitation/1",ep.local?WINHTTP_ACCESS_TYPE_NO_PROXY:WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,WINHTTP_FLAG_ASYNC);
        if(!session_){error="Cannot open WinHTTP signalling session.";return false;}
        WinHttpSetTimeouts(session_,3000,5000,5000,stream?65000:5000);
        DWORD tls=WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        if(ep.secure&&!WinHttpSetOption(session_,WINHTTP_OPTION_SECURE_PROTOCOLS,&tls,sizeof(tls))){error="TLS 1.2 is unavailable.";return false;}
        connection_=WinHttpConnect(session_,ep.host.c_str(),ep.port,0);
        const auto path=ep.path+(stream?L"/json":L"");
        request_=connection_?WinHttpOpenRequest(connection_,method,path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,ep.secure?WINHTTP_FLAG_SECURE:0):nullptr;
        if(!request_){error="Cannot create signalling request.";return false;}
        DWORD redirect=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        if(!WinHttpSetOption(request_,WINHTTP_OPTION_REDIRECT_POLICY,&redirect,sizeof(redirect))){error="Cannot restrict signalling redirects.";return false;}
        DWORD_PTR context=reinterpret_cast<DWORD_PTR>(this);
        if(!WinHttpSetOption(request_,WINHTTP_OPTION_CONTEXT_VALUE,&context,sizeof(context))){error="Cannot attach signalling request state.";return false;}
        if(WinHttpSetStatusCallback(request_,status,WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS|WINHTTP_CALLBACK_FLAG_HANDLES,0)==WINHTTP_INVALID_STATUS_CALLBACK){error="Cannot set signalling callback.";return false;}
        callback_=true;return true;
    }
    bool send(const std::string& body,std::string& error){
        if(body.size()>max_publish || cancelled()){error="Signalling cancelled or request exceeds 4096 bytes.";return false;}
        reset();const wchar_t* headers=body.empty()?L"Accept: application/x-ndjson\r\n":L"Content-Type: text/plain; charset=utf-8\r\nCache: no\r\nFirebase: no\r\n";
        if(!accepted(WinHttpSendRequest(request_,headers,DWORD(-1),body.empty()?WINHTTP_NO_REQUEST_DATA:const_cast<char*>(body.data()),DWORD(body.size()),DWORD(body.size()),reinterpret_cast<DWORD_PTR>(this)),error) || !wait(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE,std::chrono::seconds(15),error))return false;
        if(cancelled()){error="Signalling cancelled.";return false;}reset();
        if(!accepted(WinHttpReceiveResponse(request_,nullptr),error) || !wait(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE,std::chrono::seconds(15),error))return false;
        DWORD status=0,size=sizeof(status);if(!WinHttpQueryHeaders(request_,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX) || status!=200){error="Signalling server answered HTTP "+std::to_string(status)+".";return false;}return true;
    }
    bool read(char* buffer,DWORD capacity,DWORD& size,std::string& error){
        if(cancelled()){error="Signalling cancelled.";return false;}reset();
        if(!accepted(WinHttpQueryDataAvailable(request_,nullptr),error) || !wait(WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE,std::chrono::seconds(70),error))return false;
        DWORD available;{std::lock_guard lock(mutex_);available=available_;}if(!available){size=0;return true;}
        if(cancelled()){error="Signalling cancelled.";return false;}reset();capacity=std::min(capacity,available);
        if(!accepted(WinHttpReadData(request_,buffer,capacity,nullptr),error) || !wait(WINHTTP_CALLBACK_STATUS_READ_COMPLETE,std::chrono::seconds(70),error))return false;
        std::lock_guard lock(mutex_);size=read_size_;return true;
    }
};
class NtfySignalling final:public Signalling {
    mutable std::mutex state_;std::mutex join_,publishing_,starting_;std::condition_variable started_;
    std::shared_ptr<HttpRequest> receive_,post_;std::thread worker_;Endpoint endpoint_;Callback callback_;
    bool stopping_=true,start_done_=false,running_=false,used_=false,terminal_=false;std::string start_error_;
    void startup(bool ok,std::string error={}){std::lock_guard lock(state_);if(!start_done_){start_done_=true;running_=ok&&!stopping_;start_error_=std::move(error);started_.notify_all();}}
    void line(const std::string& text){
        launcher::JSONValue json;std::string error;if(!launcher::parse_json(text,json,error,{max_stream_line,max_message,96,6}))return;
        auto event=json.find("event"),message=json.find("message");if(!event||!event->string()||*event->string()!="message"||!message||!message->string())return;
        Signal value;if(!decode_signal(*message->string(),value))return;
        Callback callback;{std::lock_guard lock(state_);if(stopping_)return;callback=callback_;}
        if(callback)try{callback(std::move(value));}catch(...){/* User work never escapes the receive worker. */}
    }
    void receive_worker(){
        bool first=true;unsigned failures=0;
        for(;;){
            auto request=std::make_shared<HttpRequest>();Endpoint ep;
            {std::lock_guard lock(state_);if(stopping_){if(first){start_done_=true;started_.notify_all();}return;}receive_=request;ep=endpoint_;}
            std::string error;const auto began=Clock::now();const bool opened=request->open(ep,L"GET",true,error)&&request->send({},error);
            if(first)startup(opened,error);else if(opened){std::lock_guard lock(state_);running_=!stopping_;}
            std::array<char,4096> buffer{};
            if(opened){
                std::string pending;bool discarding=false;
                for(;;){DWORD count=0;if(!request->read(buffer.data(),DWORD(buffer.size()),count,error)||!count)break;
                    for(DWORD n=0;n<count;++n){char c=buffer[n];if(c=='\n'){if(!discarding){if(!pending.empty()&&pending.back()=='\r')pending.pop_back();if(!pending.empty())line(pending);}pending.clear();discarding=false;}
                        else if(!discarding){if(pending.size()==max_stream_line){pending.clear();discarding=true;}else pending+=c;}}
                }
            }
            // Pending asynchronous reads finish before this buffer is destroyed.
            request->close();
            std::unique_lock lock(state_);running_=false;receive_.reset();if(stopping_ || (first&&!opened))return;
            first=false;failures=opened && Clock::now()-began>=std::chrono::seconds(1)?0:std::min(failures+1,4u);
            // One worker, bounded 0.25..4s backoff, interruptible by terminal stop.
            started_.wait_for(lock,std::chrono::milliseconds(250u<<failures),[&]{return stopping_;});if(stopping_)return;
        }
    }
public:
    ~NtfySignalling() override{stop();}
    bool start(const std::string& url,const std::string& topic,Callback callback,std::string& error) override{
        std::lock_guard start_lock(starting_);error.clear();Endpoint ep;if(!callback || !endpoint(url,topic,ep,error)){if(error.empty())error="A signalling callback is required.";return false;}
        {std::lock_guard join_lock(join_);std::lock_guard lock(state_);if(terminal_||used_){error="This signalling subscription has been stopped or already started.";return false;}used_=true;endpoint_=std::move(ep);callback_=std::move(callback);stopping_=false;start_done_=running_=false;start_error_.clear();worker_=std::thread([this]{receive_worker();});}
        bool ok=false;{std::unique_lock lock(state_);if(!started_.wait_for(lock,std::chrono::seconds(20),[&]{return start_done_||stopping_;}))error="Signalling subscription timed out.";else{ok=start_done_&&running_&&!stopping_;if(!ok)error=start_error_.empty()?"Signalling subscription cancelled.":start_error_;}}
        if(!ok)stop();return ok;
    }
    bool publish(const Signal& signal,std::string& error) override{
        std::lock_guard publishing(publishing_);error.clear();if(!signal_valid(signal)){error="signal:invalid: Invalid signalling message.";return false;}
        std::string body;try{body=encode_signal(signal);}catch(...){error="signal:invalid: Signalling text is not valid UTF-8.";return false;}if(body.size()>max_publish){error="signal:oversize: Encoded signalling message exceeds ntfy's 4096-byte limit.";return false;}
        auto request=std::make_shared<HttpRequest>();Endpoint ep;
        {std::lock_guard lock(state_);if(stopping_||!running_){error="Signalling subscription is not connected.";return false;}ep=endpoint_;post_=request;}
        const bool ok=request->open(ep,L"POST",false,error)&&request->send(body,error);request->close();
        {std::lock_guard lock(state_);if(post_==request)post_.reset();}return ok;
    }
    bool connected() const override{std::lock_guard lock(state_);return running_&&!stopping_;}
    void stop() override{
        std::lock_guard join_lock(join_);std::shared_ptr<HttpRequest> receive,post;
        {std::lock_guard lock(state_);terminal_=true;stopping_=true;running_=false;receive=receive_;post=post_;started_.notify_all();}
        if(receive)receive->cancel();if(post)post->cancel();
        if(worker_.joinable())worker_.join();
        std::lock_guard publishing(publishing_);std::lock_guard lock(state_);receive_.reset();post_.reset();callback_={};
    }
};
}
std::unique_ptr<Signalling> make_signalling(){return std::make_unique<NtfySignalling>();}
}
