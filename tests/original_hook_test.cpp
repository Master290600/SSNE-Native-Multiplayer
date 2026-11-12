#include "ssne/recomp_hooks.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <thread>
namespace ssne {
std::atomic<bool> g_irq_pending{false};
void safepoint(PPCContext&) {}
}
namespace {
constexpr uint32_t address = 0x80001000;
PPCFunc hook = nullptr;
std::atomic<int> entries{0}, bodies{0};
void original(PPCContext& ctx);
void intercept(PPCContext& ctx) {
    ++entries;
    ssne_call_original(ctx, address, original);
}
void original(PPCContext& ctx) {
    SSNE_HOOK(address);
    assert(hook == intercept);
    ++bodies;
    if (ctx.r[6]) throw std::runtime_error("original failure");
    if (ctx.r[1]) { --ctx.r[1]; original(ctx); }
    if (ctx.r[7]) {
        ctx.r[7] = 0;
        std::thread other([] { PPCContext context{}; original(context); });
        other.join();
    }
}
void fail_before_entry(PPCContext&) { throw std::runtime_error("before entry"); }
}
PPCFunc* ssne_hook_slot(uint32_t a) { assert(a == address); return &hook; }
int main() {
    hook = intercept;
    for (int run=0;run<1000;++run) {
        PPCContext context{}; context.r[1]=8;
        original(context);
    }
    assert(entries==9000 && bodies==9000);
    for (int run=0;run<32;++run) {
        PPCContext context{}; context.r[7]=1;
        original(context);
    }
    assert(entries==9064 && bodies==9064);
    PPCContext context{}; context.r[6]=1;
    try { original(context); assert(false); } catch(const std::runtime_error&) {}
    try { ssne_call_original(context,address,fail_before_entry); assert(false); }
    catch(const std::runtime_error&) {}
    context.r[6]=0;
    original(context);
    assert(entries==9066 && bodies==9066 && hook==intercept);
    puts("original hooks: 1000 nested chains, 32 other-thread callbacks and exception cleanup passed");
}
