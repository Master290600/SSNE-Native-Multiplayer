// Cases for the PAL collision hooks using native_party_respawn_test's guest RAM.
namespace ground_cases {
constexpr uint32_t ground_address = 0x800C04ACu, sphere_address = 0x80140620u;
constexpr uint32_t down = 0x81100000, from = down + 16, motion = from + 16;
constexpr uint32_t extra = motion + 16, destination = extra + 16, contacts = destination + 16;
PPCFunc ground_hook = nullptr, sphere_hook = nullptr;

struct Contact {
    uint32_t kind;
    std::array<float, 3> normal;
    uint32_t flags = 0;
};
constexpr Contact road{0, {.6f, -.8f, 0.f}}, flat{4, {0.f, -1.f, 0.f}};
constexpr Contact wall{0, {1.f, 0.f, 0.f}}, steep{0, {.8f, -.6f, 0.f}};
constexpr Contact platform{2, {0.f, -1.f, 0.f}}, carried{4, {0.f, -1.f, 0.f}, 0x80000000u};
constexpr Contact trigger{3, {}};
std::vector<Contact> surfaces;
bool testing = false;
unsigned sphere_calls = 0;
float probe_y = 0, resolved_x = 0;

void sphere_entry(PPCContext& ctx) {
    if (!SSNEOriginalCall::consume(ctx, sphere_address) && sphere_hook) {
        sphere_hook(ctx);
        return;
    }
    if (!testing) {
        sphere(ctx);
        return;
    }
    ++sphere_calls;
    // Supply a known separation bias. PAL can push the sphere out by up to
    // one centimetre even when the requested movement was only a ground probe.
    mem_write32(ctx.r[6], f2u(10.005f));
    mem_write32(ctx.r[6] + 4, f2u(2.007f));
    mem_write32(ctx.r[6] + 8, f2u(-3.001f));
    for (size_t i = 0; i < surfaces.size(); ++i) {
        const uint32_t hit = ctx.r[7] + uint32_t(i) * 48u;
        for (unsigned n = 0; n < 3; ++n) mem_write32(hit + 12 + 4*n, f2u(surfaces[i].normal[n]));
        mem_write32(hit + 32, surfaces[i].flags);
        mem_write32(hit + 36, surfaces[i].kind);
    }
    ctx.r[3] = uint32_t(surfaces.size());
}

void ground_entry(PPCContext& ctx) {
    if (!SSNEOriginalCall::consume(ctx, ground_address) && ground_hook) {
        ground_hook(ctx);
        return;
    }
    probe_y = u2f(mem_read32(ctx.r[8] + 4));
    const auto start = ctx.r[7], result = ctx.r[10];
    ctx.r[4] = start;
    ctx.r[6] = result;
    ctx.r[7] = contacts;
    ctx.lr = 0x800C065Cu;
    ssne_call(ctx, sphere_address);
    require(ctx.r[3] == surfaces.size(), "The sphere hook discarded native contacts");
    for (size_t i = 0; i < surfaces.size(); ++i) {
        const uint32_t hit = contacts + uint32_t(i) * 48u;
        require(mem_read32(hit + 36) == surfaces[i].kind && mem_read32(hit + 32) == surfaces[i].flags &&
                mem_read32(hit + 16) == f2u(surfaces[i].normal[1]), "Native contact metadata changed");
    }
    resolved_x = u2f(mem_read32(result));
    // Emulate the subsequent support sweep choosing its own height.
    mem_write32(result + 4, f2u(2.03125f));
}

void check(std::vector<uint8_t>& ram) {
    fixture(ram);
    testing = true;
    mem_write32(ground_address, 0x9421FE98u);
    ssne::native_party_install_ground_collision();
    require(ground_hook && sphere_hook, "PAL collision hooks were not registered");
    mem_write32(from, f2u(10.f));
    mem_write32(from + 4, f2u(2.f));
    mem_write32(from + 8, f2u(-3.f));
    mem_write32(down + 4, f2u(-1.f));
    mem_write32(actor + 0x6D0, 1);
    auto run = [&](std::initializer_list<Contact> hits, bool expect_rest, bool expect_probe, const char* why) {
        surfaces = hits;
        auto ctx = context();
        ctx.lr = 0x800A92A8u;
        ctx.r[4] = actor; ctx.r[6] = down; ctx.r[7] = from;
        ctx.r[8] = motion; ctx.r[9] = extra; ctx.r[10] = destination;
        mem_write32(ctx.r[1] + 8, 1);
        const auto initial = std::array{mem_read32(motion), mem_read32(motion+4), mem_read32(motion+8)};
        const auto previous = sphere_calls;
        ssne_call(ctx, ground_address);
        require(sphere_calls == previous + 1, "Native sphere solver did not run exactly once");
        require(resolved_x == (expect_rest ? 10.f : 10.005f) &&
                u2f(mem_read32(destination+8)) == (expect_rest ? -3.f : -3.001f), why);
        require(probe_y == (expect_probe ? -.001f : 0.f), "Unexpected synthetic movement");
        require(u2f(mem_read32(destination+4)) == 2.03125f, "Ground probe overwrote native support height");
        require(initial == std::array{mem_read32(motion), mem_read32(motion+4), mem_read32(motion+8)},
                "Ground hook left its probe in native intent");
    };
    run({flat}, true, true, "Flat ground drifted");
    run({road, trigger, wall}, true, true, "Wall and trigger contacts destabilized a walkable slope");
    run({road, platform}, false, true, "Moving platform separation was suppressed");
    run({carried}, false, true, "A moving surface lost its native response");
    mem_write32(actor + 0x6D0, 0); mem_write32(actor + 0x6E4, 1);
    run({steep}, false, true, "A steep deliberate slide was anchored");
    mem_write32(actor + 0x6D0, 1); mem_write32(actor + 0x6E4, 0);
    mem_write32(extra, f2u(.25f));
    run({road}, false, false, "External displacement was suppressed");
    mem_write32(extra, 0); mem_write32(motion + 8, f2u(.4f));
    run({road}, false, false, "Actual walking was suppressed");
    testing = false;
}
} // namespace ground_cases
