#include "ssne/runtime.h"
#include "ssne/game_addresses.h"
using ssne::game_address;
#include "ssne/recomp_hooks.h"
#include "ssne/input.h"
#include <array>
#include <cstdio>
#include <cstdlib>

static void check(bool result, const char* message) {
    if (!result) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
int main(int argc, char** argv) {
    check(argc == 2, "Pass the local game_data directory");
    ssne::set_netplay_step([](uint64_t, uint64_t, bool, bool&) { return true; });
    PPCContext ctx;
    std::string error;
    check(ssne::boot(ctx, argv[1], error) != 0, error.c_str());
    ssne::PadState host, client;
    host.buttons = ssne::PAD_A | ssne::PAD_START;
    host.stick_x = 200; host.stick_y = 42; host.trigger_l = 255;
    client.buttons = ssne::PAD_B;
    client.cstick_x = 0; client.cstick_y = 255;
    ssne::set_pad(0, host); ssne::set_pad(1, client);
    constexpr uint32_t output = 0x81000000;
    std::array<uint8_t, 48> first;
    for (int run = 0; run < 1000; ++run) {
        // Real SIIsChanBusy data: alternate idle and an asynchronous transfer.
        for (uint32_t port = 0; port < 4; ++port)
            mem_write32(0x802D4DC0 + port * 32, run % 2 ? 0 : 0xFFFFFFFF);
        for (uint32_t i = 0; i < first.size(); ++i) mem_write8(output + i, 0xAA);
        ctx.r[3] = output;
        ssne_find_func(game_address(0x801C193C))->func(ctx);
        check(ctx.r[3] == 0xC0000000, "Incorrect connected-pad mask");
        for (uint32_t i = 0; i < first.size(); ++i) {
            if (!run) first[i] = mem_read8(output + i);
            check(mem_read8(output + i) == first[i], "SI transfer timing changed canonical input");
        }
    }
    check(mem_read16(output) == host.buttons && mem_read16(output + 12) == client.buttons,
        "Player button routing");
    check(int8_t(mem_read8(output + 2)) == 72 && int8_t(mem_read8(output + 3)) == -86,
        "Signed stick conversion");
    check(mem_read8(output + 6) == 224 && mem_read8(output + 7) == 0, "Trigger calibration");
    check(mem_read8(output + 10) == 0 && mem_read8(output + 22) == 0 &&
        mem_read8(output + 34) == 255 && mem_read8(output + 46) == 255, "Pad errors");
    // Keep the original guest deadzones and range clamping in use.
    ctx.r[3] = output;
    ssne_find_func(game_address(0x801C0BAC))->func(ctx);
    check(mem_read8(output + 10) == 0 && mem_read8(output + 22) == 0, "Guest PADClamp compatibility");
    std::array<uint8_t,48> expected;
    for(uint32_t i=0;i<expected.size();++i) expected[i]=mem_read8(output+i);
    int interrupts=0;
    ssne::queue_interrupt_work([&](PPCContext& irq) {
        ++interrupts;
        irq.r[3]=game_address(0x8028E824);
        ssne_find_func(game_address(0x801C193C))->func(irq);
        irq.r[3]=game_address(0x8028E824);
        ssne_find_func(game_address(0x801C0BAC))->func(irq);
    });
    ctx.r[13]=game_address(0x802E1BA0); ctx.msr |= 0x8000;
    ssne_find_func(game_address(0x8015544C))->func(ctx);
    check(interrupts==0,"VI callback interrupted the canonical PADRead/PADClamp pair");
    for(uint32_t i=0;i<expected.size();++i)
        check(mem_read8(game_address(0x8028E824)+i)==expected[i],"Canonical sticks were clamped twice");
    ssne::safepoint(ctx);
    check(interrupts==1,"Pending callback must still run after canonical input refresh");
    for(uint32_t i=0;i<expected.size();++i)
        check(mem_read8(game_address(0x8028E824)+i)==expected[i],"Retrace changed canonical sticks");
    // The game may consume/transform its cache during movement. Local retrace
    // must neither replace it with raw input nor calibrate it a second time.
    mem_write8(game_address(0x8028E824)+2, 35);
    mem_write8(game_address(0x8028E824)+3, 41);
    for (int run=0; run<1000; ++run) {
        ctx.r[3]=game_address(0x8028E824);
        ssne_find_func(game_address(0x801C193C))->func(ctx);
        ctx.r[3]=game_address(0x8028E824);
        ssne_find_func(game_address(0x801C0BAC))->func(ctx);
        check(mem_read8(game_address(0x8028E824)+2)==35 && mem_read8(game_address(0x8028E824)+3)==41,
              "Local retrace mutated the simulation's in-use controller cache");
    }
    ssne_find_func(game_address(0x8015544C))->func(ctx);
    for(uint32_t i=0;i<expected.size();++i)
        check(mem_read8(game_address(0x8028E824)+i)==expected[i],"Next tick did not refresh canonical input");
    std::puts("netplay pads: 1000 reads independent of SI transfer state passed");
}
