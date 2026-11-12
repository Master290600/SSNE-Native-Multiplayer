#pragma once
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace port::startup {
inline std::atomic<const char*> phase{"entry"};
inline std::atomic<unsigned long long> heartbeat{0};
inline std::atomic<unsigned> gpu_pending{0};
inline std::atomic<unsigned long long> gpu_wait_polls{0};
inline void trace(const std::string& directory, unsigned timeout_seconds=0) {
#ifdef _WIN32
    const auto pid=GetCurrentProcessId();
    const auto path=directory+"startup-"+std::to_string(pid)+".log";
    std::thread([path,pid,timeout_seconds] {
        // This file and thread do not use the game logger or renderer locks.
        FILE* file=std::fopen(path.c_str(),"w");
        const auto start=std::chrono::steady_clock::now();
        using MemoryQuery=BOOL (WINAPI*)(HANDLE,PPROCESS_MEMORY_COUNTERS,DWORD);
        const auto query_memory=reinterpret_cast<MemoryQuery>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"K32GetProcessMemoryInfo"));
        unsigned samples=0;
        while(true) {
            MEMORYSTATUSEX memory{};memory.dwLength=sizeof(memory);GlobalMemoryStatusEx(&memory);
            PROCESS_MEMORY_COUNTERS usage{};usage.cb=sizeof(usage);if(query_memory)query_memory(GetCurrentProcess(),&usage,sizeof(usage));
            FILETIME created{},exit{},kernel{},user{};GetProcessTimes(GetCurrentProcess(),&created,&exit,&kernel,&user);
            const auto ticks=[](FILETIME t){return (static_cast<unsigned long long>(t.dwHighDateTime)<<32)|t.dwLowDateTime;};
            DWORD foreground=0;GetWindowThreadProcessId(GetForegroundWindow(),&foreground);
            const auto elapsed=static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count());
            if(file) {
                std::fprintf(file,"ms=%llu pid=%lu phase=%s heartbeat=%llu cpu_ms=%llu working_mb=%llu private_mb=%llu free_mb=%llu foreground=%u gpu_pending=%u gpu_wait_polls=%llu\n",
                    elapsed,pid,phase.load(),heartbeat.load(),(ticks(kernel)+ticks(user))/10000,
                    static_cast<unsigned long long>(usage.WorkingSetSize/1048576),
                    static_cast<unsigned long long>(usage.PagefileUsage/1048576),
                    memory.ullAvailPhys/1048576,unsigned(foreground==pid),gpu_pending.load(),gpu_wait_polls.load());
                std::fflush(file);
            }
            if(timeout_seconds && elapsed>=timeout_seconds*1000ull) {
                if(file){std::fputs("Bounded startup diagnostic deadline: terminating this probe process only.\n",file);std::fclose(file);}
                TerminateProcess(GetCurrentProcess(),124);
                return;
            }
            if(!timeout_seconds && ++samples>=300){if(file)std::fclose(file);return;}
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }).detach();
#endif
}
}
