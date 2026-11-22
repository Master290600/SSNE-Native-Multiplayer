#pragma once
#include <windows.h>
#include <cstddef>
#include <cstring>

namespace port::coop::invite {

// Raw Windows XPRESS blocks have no shared dictionary and require the caller
// to supply the exact expanded size. Old systems can run the uncompressed wire.
class InviteBlockCodec {
    using Create=BOOL (WINAPI*)(DWORD,void*,HANDLE*);
    using Process=BOOL (WINAPI*)(HANDLE,void*,SIZE_T,void*,SIZE_T,SIZE_T*);
    using Close=BOOL (WINAPI*)(HANDLE);
    struct Functions {
        Create create_encoder=nullptr,create_decoder=nullptr;
        Process encode=nullptr,decode=nullptr;
        Close close_encoder=nullptr,close_decoder=nullptr;

        template<class T> static T symbol(HMODULE lib,const char* name){
            const auto addr=GetProcAddress(lib,name);T fn=nullptr;
            static_assert(sizeof(fn)==sizeof(addr));std::memcpy(&fn,&addr,sizeof(fn));return fn;
        }
        Functions(){
            // Keep a process-lifetime reference for retiring invitation workers.
            // An unavailable system32-only loader flag or API means no capability.
            HMODULE lib=LoadLibraryExW(L"cabinet.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
            if(!lib)return;
            create_encoder=symbol<Create>(lib,"CreateCompressor");
            create_decoder=symbol<Create>(lib,"CreateDecompressor");
            encode=symbol<Process>(lib,"Compress");decode=symbol<Process>(lib,"Decompress");
            close_encoder=symbol<Close>(lib,"CloseCompressor");
            close_decoder=symbol<Close>(lib,"CloseDecompressor");
        }
        bool valid()const{return create_encoder&&create_decoder&&encode&&decode&&close_encoder&&close_decoder;}
    };
    static const Functions& api(){static const Functions functions;return functions;}
    HANDLE encoder=nullptr,decoder=nullptr;
public:
    InviteBlockCodec()=default;
    InviteBlockCodec(const InviteBlockCodec&)=delete;
    InviteBlockCodec& operator=(const InviteBlockCodec&)=delete;
    ~InviteBlockCodec(){
        if(encoder)api().close_encoder(encoder);
        if(decoder)api().close_decoder(decoder);
    }
    bool open(){
        if(encoder&&decoder)return true;
        const auto& f=api();
        constexpr DWORD raw_xpress=(1u<<29)|3u; // COMPRESS_RAW | COMPRESS_ALGORITHM_XPRESS
        if(!f.valid() || !f.create_encoder(raw_xpress,nullptr,&encoder))return false;
        if(!f.create_decoder(raw_xpress,nullptr,&decoder)){f.close_encoder(encoder);encoder=nullptr;return false;}
        return true;
    }
    size_t compress(const char* src,size_t size,char* out,size_t capacity)const{
        SIZE_T written=0;
        return encoder&&api().encode(encoder,const_cast<char*>(src),size,out,capacity,&written)?written:0;
    }
    bool decompress(const char* src,size_t size,char* out,size_t expected)const{
        SIZE_T written=0;
        return decoder&&api().decode(decoder,const_cast<char*>(src),size,out,expected,&written)&&written==expected;
    }
};
}
