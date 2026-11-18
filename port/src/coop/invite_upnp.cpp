#include "invite_upnp.h"
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <objbase.h>
#include <xmllite.h>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

// Windows 7 system imports only: ws2_32, xmllite, ole32. All network sockets
// belong to the worker. Cancellation never closes another thread's handle.
namespace port::coop::invite { namespace {
using Clock=std::chrono::steady_clock;
constexpr size_t max_body=128*1024,max_headers=16*1024;
struct Failure:std::runtime_error {using std::runtime_error::runtime_error;};
struct Cancelled:Failure {Cancelled():Failure("UPnP cancelled") {}};
struct Options {unsigned discovery_ms=1500,http_timeout_ms=1200,renewal_ms=0;uint16_t ssdp_port=1900;bool loopback=false;};
#ifdef SSNE_INVITE_UPNP_TEST
std::mutex options_mutex;UpnpTestOptions test_options;
#endif
Options options(){
    Options o;
#ifdef SSNE_INVITE_UPNP_TEST
    std::lock_guard lock(options_mutex);o.ssdp_port=test_options.ssdp_port;o.loopback=true;
    o.discovery_ms=test_options.discovery_ms;o.http_timeout_ms=test_options.http_timeout_ms;o.renewal_ms=test_options.renewal_ms;
#endif
    return o;
}
bool decimal(const std::string& s,unsigned& value){
    if(s.empty() || s.size()>10)return false;const auto p=std::from_chars(s.data(),s.data()+s.size(),value);
    return p.ec==std::errc{} && p.ptr==s.data()+s.size();
}
bool ipv4(const std::string& s,uint32_t& out){
    out=0;size_t at=0;
    for(unsigned i=0;i<4;++i){const auto end=i==3?s.size():s.find('.',at);if(end==std::string::npos)return false;
        const auto part=s.substr(at,end-at);unsigned n=0;if(!decimal(part,n)||n>255||part.size()>3||(part.size()>1&&part[0]=='0'))return false;
        out=(out<<8)|n;at=end+1;}
    return at==s.size()+1 && out && (out>>24)<224 && (out>>24)!=255;
}
bool lan(uint32_t p){return (p>>24)==10 || (p>>20)==0xac1 || (p>>16)==0xc0a8 || (p>>16)==0xa9fe || (p>>22)==0x191;}
std::string ip_text(uint32_t n){return std::to_string(n>>24)+'.'+std::to_string((n>>16)&255)+'.'+std::to_string((n>>8)&255)+'.'+std::to_string(n&255);}
std::string trim(std::string s){const auto first=s.find_first_not_of(" \t\r\n"),last=s.find_last_not_of(" \t\r\n");return first==std::string::npos?std::string{}:s.substr(first,last-first+1);}
std::string lower(std::string s){for(auto& c:s)if(c>='A'&&c<='Z')c=char(c-'A'+'a');return s;}
struct Url {std::string host,path;uint16_t port=80;};
Url url(const std::string& text){
    if(text.size()>2048 || text.compare(0,7,"http://")!=0 || text.find_first_of("\r\n\t #@\\")!=std::string::npos)throw Failure("Invalid UPnP HTTP URL");
    for(unsigned char c:text)if(c<33 || c>126)throw Failure("Invalid UPnP HTTP URL");
    const auto slash=text.find_first_of("/?",7);auto authority=text.substr(7,slash==std::string::npos?std::string::npos:slash-7);Url u;
    const auto colon=authority.find(':');u.host=authority.substr(0,colon);uint32_t address=0;if(!ipv4(u.host,address))throw Failure("UPnP URL must use a numeric IPv4 address");
    if(colon!=std::string::npos){unsigned port=0;if(!decimal(authority.substr(colon+1),port)||!port||port>65535)throw Failure("Invalid UPnP HTTP port");u.port=uint16_t(port);}
    u.path=slash==std::string::npos?"/":text.substr(slash);if(u.path[0]=='?')u.path='/'+u.path;return u;
}
Url resolve(const Url& base,const std::string& relative){
    Url result;
    if(relative.compare(0,7,"http://")==0)result=url(relative);
    else{if(relative.empty() || relative.find("://")!=std::string::npos || relative.compare(0,2,"//")==0)throw Failure("Invalid UPnP control URL");
        auto path=relative[0]=='/'?relative:base.path.substr(0,base.path.find_last_of('/')+1)+relative;
        result=url("http://"+base.host+':'+std::to_string(base.port)+path);}
    if(result.host!=base.host)throw Failure("UPnP control URL changed device address");return result;
}
struct Socket {SOCKET value=INVALID_SOCKET;~Socket(){if(value!=INVALID_SOCKET)closesocket(value);}Socket()=default;Socket(const Socket&)=delete;Socket& operator=(const Socket&)=delete;};
struct Winsock {Winsock(){WSADATA data{};if(WSAStartup(MAKEWORD(2,2),&data))throw Failure("Winsock unavailable");}~Winsock(){WSACleanup();}};
std::string utf8(const wchar_t* value,unsigned count){
    if(!count)return {};const int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value,int(count),nullptr,0,nullptr,nullptr);if(n<=0)throw Failure("Invalid XML text");
    std::string out(size_t(n),'\0');WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value,int(count),out.data(),n,nullptr,nullptr);return out;
}
struct Xml {std::string name,text;std::vector<Xml> children;};
Xml parse_xml(const std::string& bytes){
    if(bytes.empty() || bytes.size()>max_body)throw Failure("Invalid UPnP XML length");
    IStream* stream=nullptr;IXmlReader* reader=nullptr;
    struct Release {IStream*& s;IXmlReader*& r;~Release(){if(r)r->Release();if(s)s->Release();}} release{stream,reader};
    const auto memory=GlobalAlloc(GMEM_MOVEABLE,bytes.size());if(!memory)throw Failure("XML allocation failed");
    void* dest=GlobalLock(memory);if(!dest){GlobalFree(memory);throw Failure("XML allocation failed");}std::memcpy(dest,bytes.data(),bytes.size());GlobalUnlock(memory);
    if(FAILED(CreateStreamOnHGlobal(memory,TRUE,&stream))){GlobalFree(memory);throw Failure("XML stream failed");}
    if(FAILED(CreateXmlReader(__uuidof(IXmlReader),reinterpret_cast<void**>(&reader),nullptr)) ||
       FAILED(reader->SetProperty(XmlReaderProperty_DtdProcessing,DtdProcessing_Prohibit)) ||
       FAILED(reader->SetProperty(XmlReaderProperty_XmlResolver,0)) ||
       FAILED(reader->SetProperty(XmlReaderProperty_MaxElementDepth,32)) || FAILED(reader->SetInput(stream)))throw Failure("XML reader failed");
    Xml root;std::vector<Xml*> stack{&root};XmlNodeType type;HRESULT hr=S_OK;unsigned elements=0;size_t text_size=0;
    while((hr=reader->Read(&type))==S_OK){
        const wchar_t* value=nullptr;UINT length=0;
        if(type==XmlNodeType_Element){if(++elements>2048 || stack.size()>32 || FAILED(reader->GetLocalName(&value,&length)))throw Failure("UPnP XML too complex");
            stack.back()->children.push_back({utf8(value,length),{}, {}});auto* child=&stack.back()->children.back();if(!reader->IsEmptyElement())stack.push_back(child);
        }else if(type==XmlNodeType_EndElement){if(stack.size()<2)throw Failure("Invalid UPnP XML nesting");stack.pop_back();}
        else if(type==XmlNodeType_Text || type==XmlNodeType_CDATA || type==XmlNodeType_Whitespace){
            if(FAILED(reader->GetValue(&value,&length)))throw Failure("Invalid UPnP XML text");auto s=utf8(value,length);text_size+=s.size();if(text_size>max_body)throw Failure("UPnP XML too large");stack.back()->text+=s;
        }else if(type==XmlNodeType_DocumentType)throw Failure("UPnP XML DTD forbidden");
    }
    if(hr!=S_FALSE || stack.size()!=1 || root.children.size()!=1)throw Failure("Malformed UPnP XML");return std::move(root.children[0]);
}
void find_nodes(const Xml& node,const std::string& name,std::vector<const Xml*>& out){if(node.name==name)out.push_back(&node);for(const auto& c:node.children)find_nodes(c,name,out);}
std::string leaf(const Xml& node,const std::string& name){std::vector<const Xml*> found;find_nodes(node,name,found);if(found.size()>1)throw Failure("Ambiguous UPnP XML field");return found.empty()?std::string{}:trim(found[0]->text);}
struct Service {Url control;std::string type;};
std::vector<Service> services(const std::string& bytes,const Url& location){
    const auto xml=parse_xml(bytes);Url base=location;const auto custom_base=leaf(xml,"URLBase");if(!custom_base.empty()){base=url(custom_base);if(base.host!=location.host)throw Failure("UPnP base URL changed address");}
    std::vector<const Xml*> found;find_nodes(xml,"service",found);std::vector<Service> result;
    for(const auto* n:found){const auto type=leaf(*n,"serviceType");if(type=="urn:schemas-upnp-org:service:WANIPConnection:2" || type=="urn:schemas-upnp-org:service:WANIPConnection:1" || type=="urn:schemas-upnp-org:service:WANPPPConnection:1")result.push_back({resolve(base,leaf(*n,"controlURL")),type});}
    auto rank=[](const std::string& type){return type=="urn:schemas-upnp-org:service:WANIPConnection:2"?0:type=="urn:schemas-upnp-org:service:WANIPConnection:1"?1:2;};
    std::stable_sort(result.begin(),result.end(),[&](const auto& a,const auto& b){return rank(a.type)<rank(b.type);});return result;
}
std::string xml_escape(const std::string& s){std::string out;for(char c:s)switch(c){case '&':out+="&amp;";break;case '<':out+="&lt;";break;case '>':out+="&gt;";break;default:out+=c;}return out;}
using Fields=std::vector<std::pair<std::string,std::string>>;
std::string envelope(const Service& s,const std::string& action,const Fields& args){
    std::string xml="<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:"+action+" xmlns:u=\""+s.type+"\">";
    for(const auto& [name,value]:args)xml+='<'+name+'>'+xml_escape(value)+"</"+name+'>';return xml+"</u:"+action+"></s:Body></s:Envelope>";
}
std::string token(){GUID g{};if(FAILED(CoCreateGuid(&g)))throw Failure("Mapping identity unavailable");constexpr char hex[]="0123456789abcdef";std::string out="SSNE-";for(unsigned char c:reinterpret_cast<const unsigned char(&)[16]>(g)){out+=hex[c>>4];out+=hex[c&15];}return out;}
struct HttpReply {unsigned status=0;std::string body;};
enum class Decode {More,Done};
Decode chunks(const std::string& input,std::string& output){
    output.clear();size_t at=0;
    for(unsigned count=0;count<4096;++count){const auto end=input.find("\r\n",at);if(end==std::string::npos)return Decode::More;
        auto size=input.substr(at,end-at);const auto ext=size.find(';');if(ext!=std::string::npos)size.resize(ext);if(size.empty() || size.size()>8)throw Failure("Invalid HTTP chunk");
        unsigned n=0;const auto p=std::from_chars(size.data(),size.data()+size.size(),n,16);if(p.ec!=std::errc{}||p.ptr!=size.data()+size.size()||n>max_body-output.size())throw Failure("HTTP body too large");at=end+2;
        if(!n){if(input.size()<at+2)return Decode::More;if(input.compare(at,2,"\r\n")==0)return Decode::Done;if(input.find("\r\n\r\n",at)!=std::string::npos)return Decode::Done;return Decode::More;}
        if(input.size()<at+n+2)return Decode::More;if(input.compare(at+n,2,"\r\n")!=0)throw Failure("Invalid HTTP chunk ending");output.append(input,at,n);at+=n+2;
    }throw Failure("Too many HTTP chunks");
}
} // namespace

struct PortMapping::Impl {
    mutable std::mutex state_mutex;std::mutex lifecycle;std::condition_variable wake;std::mutex wake_mutex;
    MappingStatus state;std::thread worker;std::atomic<bool> cancel{false};
    void check(bool cleanup=false)const{if(!cleanup && cancel.load(std::memory_order_acquire))throw Cancelled();}
    void publish(MappingStatus s){std::lock_guard lock(state_mutex);state=std::move(s);}
    bool wait_socket(SOCKET socket,bool write,Clock::time_point deadline,bool cleanup){
        while(Clock::now()<deadline){check(cleanup);fd_set ready;FD_ZERO(&ready);FD_SET(socket,&ready);timeval time{0,25000};
            const auto n=select(0,write?nullptr:&ready,write?&ready:nullptr,nullptr,&time);if(n==SOCKET_ERROR)throw Failure("UPnP socket wait failed");if(n)return true;}
        throw Failure("UPnP HTTP timeout");
    }
    HttpReply http(const Url& u,const std::string& method,const std::string& headers,const std::string& body,const Options& o,bool cleanup=false){
        check(cleanup);Socket socket;socket.value=::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(socket.value==INVALID_SOCKET)throw Failure("UPnP HTTP socket failed");
        u_long nonblocking=1;if(ioctlsocket(socket.value,FIONBIO,&nonblocking))throw Failure("UPnP HTTP setup failed");uint32_t ip=0;if(!ipv4(u.host,ip))throw Failure("Invalid device address");
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(u.port);address.sin_addr.s_addr=htonl(ip);const auto deadline=Clock::now()+std::chrono::milliseconds(o.http_timeout_ms);
        if(connect(socket.value,reinterpret_cast<sockaddr*>(&address),sizeof(address))==SOCKET_ERROR){const int error=WSAGetLastError();if(error!=WSAEWOULDBLOCK&&error!=WSAEINPROGRESS)throw Failure("UPnP HTTP connect failed");
            wait_socket(socket.value,true,deadline,cleanup);int result=0;int length=sizeof(result);if(getsockopt(socket.value,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&result),&length)||result)throw Failure("UPnP HTTP connect failed");}
        const std::string request=method+' '+u.path+" HTTP/1.1\r\nHost: "+u.host+':'+std::to_string(u.port)+"\r\nUser-Agent: SSNE/1 UPnP/1.1\r\nConnection: close\r\nAccept-Encoding: identity\r\n"+headers+"Content-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;
        for(size_t at=0;at<request.size();){wait_socket(socket.value,true,deadline,cleanup);const auto n=send(socket.value,request.data()+at,int(request.size()-at),0);if(n>0)at+=size_t(n);else if(n==0||WSAGetLastError()!=WSAEWOULDBLOCK)throw Failure("UPnP HTTP send failed");}
        std::string input;HttpReply reply;size_t start=0;unsigned content_length=0;bool has_length=false,chunked=false,parsed=false;
        for(;;){
            check(cleanup);if(parsed){const auto payload=input.substr(start);if(chunked){if(chunks(payload,reply.body)==Decode::Done)return reply;}
                else if(has_length && payload.size()>=content_length){reply.body=payload.substr(0,content_length);return reply;}}
            wait_socket(socket.value,false,deadline,cleanup);char buffer[8192];const auto n=recv(socket.value,buffer,sizeof(buffer),0);
            if(!n){if(!parsed || chunked || (has_length && input.size()-start<content_length))throw Failure("Incomplete UPnP HTTP response");reply.body=input.substr(start);return reply;}
            if(n<0){if(WSAGetLastError()==WSAEWOULDBLOCK)continue;throw Failure("UPnP HTTP receive failed");}
            input.append(buffer,size_t(n));if(input.size()>max_body+max_headers)throw Failure("UPnP HTTP response too large");
            if(!parsed){const auto end=input.find("\r\n\r\n");if(end==std::string::npos){if(input.size()>max_headers)throw Failure("UPnP HTTP headers too large");continue;}
                if(end>max_headers)throw Failure("UPnP HTTP headers too large");const auto line=input.find("\r\n");if(line==std::string::npos || (input.compare(0,9,"HTTP/1.1 ")!=0&&input.compare(0,9,"HTTP/1.0 ")!=0) || !decimal(input.substr(9,3),reply.status))throw Failure("Invalid UPnP HTTP status");
                std::map<std::string,std::string> fields;for(size_t at=line+2;at<end;){const auto next=input.find("\r\n",at),colon=input.find(':',at);if(colon==std::string::npos||colon>=next)throw Failure("Invalid HTTP header");const auto name=lower(input.substr(at,colon-at));if(!fields.emplace(name,trim(input.substr(colon+1,next-colon-1))).second)throw Failure("Duplicate HTTP header");at=next+2;}
                if(const auto f=fields.find("content-length");f!=fields.end()){has_length=true;if(!decimal(f->second,content_length)||content_length>max_body)throw Failure("Invalid HTTP length");}
                if(const auto f=fields.find("transfer-encoding");f!=fields.end()){if(lower(f->second)!="chunked"||has_length)throw Failure("Unsupported HTTP encoding");chunked=true;}
                if(const auto f=fields.find("content-encoding");f!=fields.end()&&lower(f->second)!="identity")throw Failure("Unsupported HTTP compression");start=end+4;parsed=true;
            }
            if(parsed && input.size()-start>max_body)throw Failure("UPnP HTTP body too large");
        }
    }
    std::vector<Url> discover(const std::string& local,const Options& o){
        Socket socket;socket.value=::socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);if(socket.value==INVALID_SOCKET)throw Failure("UPnP discovery socket failed");
        u_long nonblocking=1;if(ioctlsocket(socket.value,FIONBIO,&nonblocking))throw Failure("UPnP discovery setup failed");uint32_t local_ip=0;ipv4(local,local_ip);
        sockaddr_in here{};here.sin_family=AF_INET;here.sin_addr.s_addr=htonl(local_ip);if(bind(socket.value,reinterpret_cast<sockaddr*>(&here),sizeof(here)))throw Failure("UPnP discovery bind failed");
        if(!o.loopback && setsockopt(socket.value,IPPROTO_IP,IP_MULTICAST_IF,reinterpret_cast<const char*>(&here.sin_addr),sizeof(here.sin_addr)))throw Failure("UPnP discovery interface failed");
        const DWORD ttl=1;setsockopt(socket.value,IPPROTO_IP,IP_MULTICAST_TTL,reinterpret_cast<const char*>(&ttl),sizeof(ttl));
        sockaddr_in target{};target.sin_family=AF_INET;target.sin_port=htons(o.ssdp_port);target.sin_addr.s_addr=htonl(o.loopback?0x7f000001u:0xeffffffau);
        for(const char* type:{"urn:schemas-upnp-org:device:InternetGatewayDevice:1","urn:schemas-upnp-org:device:InternetGatewayDevice:2","urn:schemas-upnp-org:service:WANIPConnection:1"}){
            const std::string request="M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 1\r\nST: "+std::string(type)+"\r\n\r\n";check();if(sendto(socket.value,request.data(),int(request.size()),0,reinterpret_cast<sockaddr*>(&target),sizeof(target))==SOCKET_ERROR)throw Failure("UPnP discovery send failed "+std::to_string(WSAGetLastError()));}
        auto deadline=Clock::now()+std::chrono::milliseconds(o.discovery_ms);std::vector<Url> result;std::set<std::string> seen;
        while(Clock::now()<deadline && result.size()<8){check();fd_set ready;FD_ZERO(&ready);FD_SET(socket.value,&ready);timeval timeout{0,25000};if(select(0,&ready,nullptr,nullptr,&timeout)<=0)continue;
            char bytes[4096];sockaddr_in peer{};int size=sizeof(peer);const auto n=recvfrom(socket.value,bytes,sizeof(bytes),0,reinterpret_cast<sockaddr*>(&peer),&size);if(n<=0)continue;std::string response(bytes,size_t(n));
            if(response.compare(0,12,"HTTP/1.1 200")!=0 && response.compare(0,12,"HTTP/1.0 200")!=0)continue;
            const auto sender=ntohl(peer.sin_addr.s_addr);if(o.loopback?sender!=0x7f000001u:!lan(sender))continue;
            for(size_t at=response.find("\r\n")+2;at<response.size();){const auto end=response.find("\r\n",at);if(end==std::string::npos||end==at)break;const auto colon=response.find(':',at);
                if(colon<end && lower(response.substr(at,colon-at))=="location")try{const auto location=trim(response.substr(colon+1,end-colon-1));const auto parsed=url(location);if(parsed.host==ip_text(sender)&&seen.insert(location).second){result.push_back(parsed);deadline=std::min(deadline,Clock::now()+std::chrono::milliseconds(100));}}catch(const Failure&){}at=end+2;}
        }
        if(result.empty())throw Failure("No UPnP IGD found");return result;
    }
    struct Soap {Xml xml;unsigned error=0;};
    Soap soap(const Service& s,const std::string& action,const Fields& fields,const Options& o,bool cleanup=false){
        const auto body=envelope(s,action,fields);const auto reply=http(s.control,"POST","Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \""+s.type+'#'+action+"\"\r\n",body,o,cleanup);
        Soap result{parse_xml(reply.body),0};const auto fault=leaf(result.xml,"errorCode");if(!fault.empty()){if(!decimal(fault,result.error)||!result.error)throw Failure("Invalid UPnP SOAP fault");return result;}
        if(reply.status!=200)throw Failure("UPnP HTTP error "+std::to_string(reply.status));std::vector<const Xml*> response;find_nodes(result.xml,action+"Response",response);if(response.size()!=1)throw Failure("Unexpected UPnP SOAP response");return result;
    }
    struct Entry {unsigned error=0,lease=0;std::string client,port,description,enabled;};
    Fields key(uint16_t port){return {{"NewRemoteHost",""},{"NewExternalPort",std::to_string(port)},{"NewProtocol","UDP"}};}
    Entry entry(const Service& s,uint16_t port,const Options& o,bool cleanup=false){
        auto answer=soap(s,"GetSpecificPortMappingEntry",key(port),o,cleanup);Entry result;result.error=answer.error;if(result.error)return result;
        result.client=leaf(answer.xml,"NewInternalClient");result.port=leaf(answer.xml,"NewInternalPort");result.description=leaf(answer.xml,"NewPortMappingDescription");result.enabled=leaf(answer.xml,"NewEnabled");
        if(!decimal(leaf(answer.xml,"NewLeaseDuration"),result.lease))throw Failure("Invalid UPnP lease response");return result;
    }
    bool own(const Entry& e,const std::string& local,uint16_t port,const std::string& identity){return !e.error && e.client==local && e.port==std::to_string(port) && e.description==identity;}
    void add(const Service& s,const std::string& local,uint16_t port,const std::string& identity,const Options& o){
        auto args=key(port);args.insert(args.end(),{{"NewInternalPort",std::to_string(port)},{"NewInternalClient",local},{"NewEnabled","1"},{"NewPortMappingDescription",identity},{"NewLeaseDuration","120"}});
        const auto result=soap(s,"AddPortMapping",args,o);if(result.error)throw Failure("UPnP AddPortMapping error "+std::to_string(result.error));
    }
    void cleanup(const Service& s,const std::string& local,uint16_t port,const std::string& identity,const Options& o)noexcept{
        // Quit must not wait for a slow/missing gateway. A failed conditional
        // cleanup leaves only the already requested finite120-second lease.
        auto bounded=o;bounded.http_timeout_ms=std::min(o.http_timeout_ms,100u);
        try{if(!own(entry(s,port,bounded,true),local,port,identity))return;(void)soap(s,"DeletePortMapping",key(port),bounded,true);}catch(...){}
    }
    void run(std::string local,uint16_t port,Options o){
        std::unique_ptr<Service> selected;std::string identity;bool attempted=false;std::string external;std::string failure;
        try{
            Winsock winsock;identity=token();const auto locations=discover(local,o);std::vector<Service> candidates;
            for(const auto& location:locations){check();try{auto answer=http(location,"GET",{}, {},o);if(answer.status!=200)continue;auto list=services(answer.body,location);candidates.insert(candidates.end(),list.begin(),list.end());}catch(const Cancelled&){throw;}catch(const Failure&){}}
            if(candidates.empty())throw Failure("No supported UPnP WAN service");
            for(const auto& candidate:candidates){check();const auto ext=soap(candidate,"GetExternalIPAddress",{},o);if(ext.error)continue;uint32_t ip=0;external=leaf(ext.xml,"NewExternalIPAddress");if(!ipv4(external,ip))continue;selected=std::make_unique<Service>(candidate);break;}
            if(!selected)throw Failure("UPnP external IPv4 unavailable");
            const auto before=entry(*selected,port,o);if(before.error!=714)throw Failure(before.error?"UPnP mapping lookup error "+std::to_string(before.error):"UDP port already has a mapping; existing entry preserved");
            check();attempted=true;add(*selected,local,port,identity,o);auto current=entry(*selected,port,o);if(!own(current,local,port,identity)||current.enabled!="1"||!current.lease||current.lease>120)throw Failure("Router did not confirm the requested finite owned lease");
            publish({true,true,external,{},port});
            for(;;){
                const auto duration=std::chrono::milliseconds(o.renewal_ms?o.renewal_ms:std::max(100u,current.lease*1000u/3u));std::unique_lock lock(wake_mutex);if(wake.wait_for(lock,duration,[&]{return cancel.load();}))throw Cancelled();lock.unlock();
                current=entry(*selected,port,o);if(!own(current,local,port,identity))throw Failure("UPnP mapping ownership changed; entry preserved");
                if(!current.lease)throw Failure("UPnP finite lease was replaced");check();add(*selected,local,port,identity,o);current=entry(*selected,port,o);
                if(!own(current,local,port,identity)||current.enabled!="1"||!current.lease||current.lease>120)throw Failure("UPnP lease renewal was not confirmed");
            }
        }catch(const Cancelled&){failure="UPnP cancelled";}catch(const std::exception& e){failure=e.what();}catch(...){failure="UPnP mapping failed";}
        // A timed-out Add may still have succeeded. Query the exact token even
        // after cancellation; never remove a foreign entry or an unverified one.
        if(attempted && selected){try{Winsock winsock;cleanup(*selected,local,port,identity,o);}catch(...){}}
        publish({true,false,external,std::move(failure),0});
    }
    void stop_worker(){cancel.store(true,std::memory_order_release);wake.notify_all();if(worker.joinable())worker.join();}
};

PortMapping::PortMapping():impl_(std::make_unique<Impl>()){}
PortMapping::~PortMapping(){stop();}
bool PortMapping::start(std::string local,uint16_t port){
    std::lock_guard lock(impl_->lifecycle);impl_->stop_worker();uint32_t ip=0;const auto o=options();
    if(!port || !ipv4(local,ip) || (!o.loopback && (ip>>24)==127) || (o.loopback && (ip!=0x7f000001u || !o.ssdp_port))){impl_->publish({true,false,{},"Invalid local IPv4 or UDP port for UPnP",0});return false;}
    impl_->cancel.store(false,std::memory_order_release);impl_->publish({});try{impl_->worker=std::thread([p=impl_.get(),local=std::move(local),port,o]{p->run(local,port,o);});return true;}catch(...){impl_->publish({true,false,{},"UPnP worker could not start",0});return false;}
}
MappingStatus PortMapping::status()const{std::lock_guard lock(impl_->state_mutex);return impl_->state;}
void PortMapping::request_stop(){impl_->cancel.store(true,std::memory_order_release);impl_->wake.notify_all();}
void PortMapping::stop(){std::lock_guard lock(impl_->lifecycle);impl_->stop_worker();}
#ifdef SSNE_INVITE_UPNP_TEST
void set_upnp_test_options(UpnpTestOptions o){std::lock_guard lock(options_mutex);test_options=o;}
#endif
} // namespace port::coop::invite
