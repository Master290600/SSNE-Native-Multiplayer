// Opt-in integration probe: use the real file loader at CcEngine::Flip.
void savestate_checkpoint_probe(PPCContext& ctx) {
    static bool done = false;
    static bool await_resume = false;
    static uint64_t resume_tb = 0, resume_vi = 0, resume_ai = 0, resume_generation = 0;
    static std::chrono::steady_clock::time_point resume_started;
    if (!getenv("SSNE_TEST_HIDDEN") || !getenv("SSNE_TEST_RUNTIME_CHECKPOINT")) return;
    if (done) {
        if (await_resume && std::chrono::steady_clock::now() - resume_started >= std::chrono::milliseconds(150)) {
            const auto now_tb = ssne_timebase(), vi = scheduler_stats().retraces, ai = audio_stats().ai_frames;
            if (now_tb <= resume_tb || vi <= resume_vi || (resume_ai && ai <= resume_ai) || runtime_state_generation() <= resume_generation) fatal("Runtime producers did not resume after checkpoint");
            std::ofstream audit("runtime-quiescence-resumed.json");
            audit << "{\"clock_ticks\":" << now_tb-resume_tb << ",\"retraces\":" << vi-resume_vi << ",\"audio_frames\":" << ai-resume_ai << ",\"generation\":" << runtime_state_generation() << "}\n";
            if (!audit) fatal("Cannot write producer resume evidence");
            log_message("[runtime-checkpoint-resumed] retraces=" + std::to_string(vi-resume_vi) + " audio_frames=" + std::to_string(ai-resume_ai));
            await_resume = false;
        }
        return;
    }
    if (mem_read32(0x802DB7E8) != 4) return;
    // Scene 4 is also set during level import. Wait for initialized native
    // player actors and 120 further frames before exercising a gameplay save.
    static unsigned ready_frames = 0;
    const auto players = mem_read32(0x802DB8CC) == 0 ? 1u : native_party_capacity();
    const auto registry = native_party_registry();
    for (unsigned slot = 0; slot < players; ++slot) {
        const auto actor = mem_read32(registry + slot * 4);
        if (actor < 0x80000000u || actor > guest_ram_end() - 0x790 || mem_read32(actor + 0x48) != 0x802121E8u || mem_read32(actor + 0x17C) != slot) { ready_frames = 0; return; }
    }
    if (++ready_frames < 120) return;
    RuntimeStateGuard guard;
    std::string why;
    if (!sched_state_check(why)) return;
    done = true;
#ifdef _WIN32
    if (getenv("SSNE_TEST_CHECKPOINT_DIRTY_CARD")) {
        // Exercise a real failed Windows write with unchanged card bytes.
        // Restore the emulated bus afterwards, preserving only the pending
        // backing-file write that a valid checkpoint load must commit.
        StateWriter bus; exi_state_save(bus);
        const auto bytes = exi_memcard_snapshot();
        const auto path = std::filesystem::u8path(exi_memcard_path());
        const auto handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) fatal("Cannot lock checkpoint fixture card");
        auto reg = [](uint32_t off, uint32_t value) { exi_write16(off, uint16_t(value >> 16)); exi_write16(off + 2, uint16_t(value)); };
        auto send = [&](uint8_t value) { reg(0x6810, uint32_t(value) << 24); reg(0x680C, 5); };
        reg(0x6800, 128); send(0xF2); send(0); send(0); send(2); send(0);
        for (unsigned i=0; i<128; ++i) send(bytes.at(0x100+i));
        reg(0x6800, 0);
        reg(0x6800, 128); send(0x83); reg(0x680C, 1);
        const auto status = uint8_t(exi_read16(0x6810) >> 8); reg(0x6800, 0);
        CloseHandle(handle);
        StateReader restore{bus.buf.data(), bus.buf.size()};
        if (!(status & 8) || exi_memcard_snapshot() != bytes || !exi_state_load_volatile(restore)) fatal("Dirty-card fixture did not preserve emulated state after write failure");
        log_message("[checkpoint-dirty-card] real_write_failure=1 card_bytes_unchanged=1");
    }
#endif
    const std::string path = "runtime-checkpoint-probe.state", bad_path = "runtime-checkpoint-invalid.state";
    auto require = [](bool ok, const char* message) { if (!ok) fatal(message); };
    auto capture_modules = [] {
        StateWriter bytes;
        sched_state_save(bytes); hw_state_save(bytes); si_state_save(bytes); exi_state_save(bytes); gx_state_save(bytes);
        return bytes.buf;
    };
    const auto frozen_tb = ssne_timebase(), frozen_vi = scheduler_stats().retraces, frozen_ai = audio_stats().ai_frames;
    const auto frozen_generation = runtime_state_generation();
    const auto frozen_modules = capture_modules();
    {
        RuntimeStateGuard nested;
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }
    require(ssne_timebase() == frozen_tb && scheduler_stats().retraces == frozen_vi && audio_stats().ai_frames == frozen_ai, "Background producers ran inside checkpoint barrier");
    require(capture_modules() == frozen_modules && runtime_state_generation() == frozen_generation, "Checkpoint module state changed inside nested barrier");
    SavestateResult result;
    struct ProbeResultScope {
        explicit ProbeResultScope(SavestateResult& result) { g_probe_result = &result; }
        ~ProbeResultScope() { g_probe_result = nullptr; }
    } result_scope(result);
    do_save(ctx, path);
    require(result.save && result.ok, "Runtime checkpoint probe could not save");
    std::ifstream file(path, std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), {});
    StateReader header{data.data(), data.size()};
    header.bytes(nullptr, 26); const auto count = header.u32(); header.bytes(nullptr, count * 4);
    header.u64(); header.u64(); const auto crc_at = header.pos; header.u32(); const auto length_at = header.pos; header.u64();
    const auto payload_at = header.pos;
    StateReader body{data.data() + payload_at, data.size() - payload_at};
    std::vector<size_t> tags{payload_at};
    require(body.tag("CPU1") && body.bytes(nullptr, sizeof(PPCContext)), "Missing CPU probe section");
    tags.push_back(payload_at + body.pos);
    require(body.tag("RAM1"), "Missing RAM probe section");
    const auto first_flag = payload_at + body.pos + 8;
    require(data.at(first_flag) == 1, "Expected original nonzero game-ID RAM page");
    require(body.block(nullptr, guest_ram_size()>0x02000000?kRamSize:0x02000000) && body.bytes(nullptr, kLockedCacheSize), "Invalid saved probe RAM");
    for (auto load : {sched_state_load, hw_state_load, si_state_load, exi_state_load, gx_state_load}) {
        tags.push_back(payload_at + body.pos);
        require(load(body, false), "Valid native section failed validation");
    }
    require(body.ok && body.pos == body.n, "Probe payload has trailing data");
    const auto original_ctx = ctx;
    const size_t ram_size = guest_ram_size()>0x02000000?kRamSize:0x02000000;
    const std::vector<uint8_t> ram(g_ram, g_ram + ram_size), cache(g_locked_cache, g_locked_cache + kLockedCacheSize);
    StateWriter card; exi_state_save(card);
    auto card_writes = exi_memcard_writes();
    unsigned rejected = 0;
    auto reject = [&](std::vector<uint8_t> bad) {
        // A valid CRC and modified earlier RAM section ensure late rejection
        // cannot be mistaken for a checksum-only failure.
        bad.at(first_flag + 1 + 0x100) ^= 0x5A;
        const uint64_t length = bad.size() - payload_at;
        const auto crc = crc32(bad.data() + payload_at, size_t(length));
        memcpy(bad.data() + crc_at, &crc, sizeof(crc)); memcpy(bad.data() + length_at, &length, sizeof(length));
        if(!write_file(bad_path,bad,why))fatal(("Could not write malformed probe state: "+why).c_str());
        do_load(ctx, bad_path);
        require(!result.save && !result.ok, "Malformed runtime state accepted");
        require(!memcmp(&ctx, &original_ctx, sizeof(ctx)) && !memcmp(g_ram, ram.data(), ram_size) && !memcmp(g_locked_cache, cache.data(), cache.size()), "Rejected state changed CPU/RAM/cache");
        StateWriter after; exi_state_save(after);
        require(after.buf == card.buf && exi_memcard_writes() == card_writes, "Rejected state changed or flushed the native card");
        ++rejected;
    };
    for (auto tag : tags) { auto bad = data; bad.at(tag) ^= 1; reject(std::move(bad)); }
    { auto bad = data; bad.at(first_flag) = 2; reject(std::move(bad)); }
    { auto bad = data; bad.push_back(0xAD); reject(std::move(bad)); }
    { auto bad = data; std::fill_n(bad.begin() + 26, 4, 0xFF); reject(std::move(bad)); }
    // Positive control: restore an actual changed register and RAM byte, then
    // continue this same original frame. This is not a whole-session rewind.
    ctx.r[3] ^= 0x12345678u; g_ram[0x4000] ^= 0xA5; g_locked_cache[0] ^= 0x5A;
    do_load(ctx, path);
    require(!result.save && result.ok, "Valid native runtime state did not load");
    const auto valid_load_writes = exi_memcard_writes() - card_writes;
    log_message("[checkpoint-valid-load] writes_before=" + std::to_string(card_writes) + " writes_after=" + std::to_string(exi_memcard_writes()));
    require(valid_load_writes <= 1, "Valid checkpoint load persisted the card more than once");
    if (getenv("SSNE_TEST_CHECKPOINT_DIRTY_CARD")) require(valid_load_writes == 1, "Valid load did not commit the pending fixture card write");
    // The successful load may legitimately persist an earlier dirty range.
    // Failed transactions below must preserve the state AFTER that commit.
    card_writes = exi_memcard_writes();
    require(!memcmp(&ctx, &original_ctx, sizeof(ctx)) && !memcmp(g_ram, ram.data(), ram_size) && !memcmp(g_locked_cache, cache.data(), cache.size()), "Valid runtime state did not restore CPU/RAM/cache");
    require(ssne_timebase() == frozen_tb && runtime_state_generation() == frozen_generation, "Nested file operations resumed the frozen clock");
    // A valid core image reaches the participant only after RAM/CPU application.
    // Reject a re-signed bad native binding there, then exercise an exception
    // after a successful participant application. Both must restore everything.
    std::vector<uint8_t> party;
    require(native_party_save_state(party, why), "Cannot capture transaction party");
    auto invalid_party = party;
    require(invalid_party.size() > 284, "Missing actor binding in party checkpoint");
    std::fill_n(invalid_party.begin() + 280, 4, 0xFF);
    uint64_t hash = 14695981039346656037ull;
    for (size_t i=0; i<invalid_party.size()-8; ++i) { hash ^= invalid_party[i]; hash *= 1099511628211ull; }
    for (unsigned i=0; i<8; ++i) invalid_party[invalid_party.size()-8+i] = uint8_t(hash >> (56-i*8));
    const auto disk_path = exi_memcard_path();
    auto disk_card = [&] { std::ifstream f(std::filesystem::u8path(disk_path), std::ios::binary); return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {}); };
    const auto original_disk = disk_card();
    const auto exi_memcard_snapshot_before_transaction = exi_memcard_snapshot();
    log_message("[transaction-card-before] disk_bytes=" + std::to_string(original_disk.size()) + " memory_bytes=" + std::to_string(exi_memcard_snapshot_before_transaction.size()) + " equal=" + std::to_string(original_disk == exi_memcard_snapshot_before_transaction) + " writes=" + std::to_string(card_writes));
    auto changed_card_target = data;
    // The paged card block is the EXI section's final field. Locate its exact
    // serialized suffix rather than assuming the ABI size of channel structs.
    StateWriter card_block;
    card_block.block(exi_memcard_snapshot_before_transaction.data(), exi_memcard_snapshot_before_transaction.size());
    require(tags.at(6) >= card_block.buf.size() && std::equal(card_block.buf.begin(), card_block.buf.end(), data.begin()+tags.at(6)-card_block.buf.size()), "Cannot locate serialized card block");
    const auto card_flag = tags.at(6) - card_block.buf.size() + 8;
    require(changed_card_target.at(card_flag) == 1, "Expected populated card header page");
    changed_card_target.at(card_flag + 1 + 0x100) ^= 0x39;
    const auto changed_card_crc = crc32(changed_card_target.data()+payload_at, changed_card_target.size()-payload_at);
    memcpy(changed_card_target.data()+crc_at, &changed_card_crc, sizeof(changed_card_crc));
    PadState changed_input{}; changed_input.buttons = 0x100; changed_input.stick_x = 17;
    native_party_set_input(players-1, changed_input);
    std::vector<uint8_t> changed_party;
    require(native_party_save_state(changed_party, why) && changed_party != party, "Party positive control did not change input");
    unsigned rollback_cases = 0;
    for (unsigned mode=0; mode<3; ++mode) {
        ctx.r[3] ^= 0x197234ABu; g_ram[0x4000] ^= 0xA7; g_locked_cache[0] ^= 0x5B;
        const auto changed_ctx = ctx;
        const auto changed_ram = g_ram[0x4000], changed_cache = g_locked_cache[0];
        const auto before_modules = capture_modules();
        const auto blocked_temp = std::filesystem::u8path(disk_path + ".state-tmp");
        if (mode == 2) require(!disk_path.empty() && std::filesystem::create_directory(blocked_temp), "Cannot create fixture-only card write obstruction");
        unsigned calls = 0;
        auto restore = [&](const std::vector<uint8_t>& bytes, std::string& error) {
            if (++calls == 1) {
                require(!memcmp(&ctx, &original_ctx, sizeof(ctx)) && g_ram[0x4000] == ram[0x4000] && g_locked_cache[0] == cache[0], "Participant ran before target RAM application");
                require(exi_memcard_snapshot() != exi_memcard_snapshot_before_transaction && disk_card() == original_disk, "Target card was not staged exclusively in memory");
                if (mode == 1) {
                    require(native_party_load_state(bytes, error), "Valid party application failed");
                    throw std::runtime_error("Injected post-participant failure");
                }
            }
            return native_party_load_state(bytes, error);
        };
        require(!load_transaction(ctx, changed_card_target, mode ? party : invalid_party, native_party_save_state, restore, why), "Invalid combined checkpoint accepted");
        if (mode == 2) {
            require(why == "Cannot write checkpoint card image", "Card persistence failure was not reached");
            require(std::filesystem::remove(blocked_temp), "Cannot clear fixture-only card obstruction");
        }
        require(calls == 2 && !memcmp(&ctx, &changed_ctx, sizeof(ctx)) && g_ram[0x4000] == changed_ram && g_locked_cache[0] == changed_cache, "Transaction failed to restore pre-load runtime");
        g_ram[0x4000] ^= 0xA7; g_locked_cache[0] ^= 0x5B; ctx.r[3] ^= 0x197234ABu;
        require(!memcmp(g_ram, ram.data(), ram_size) && !memcmp(g_locked_cache, cache.data(), cache.size()) && before_modules == capture_modules(), "Rollback changed other RAM/cache/hardware");
        std::vector<uint8_t> after_party;
        require(native_party_save_state(after_party, why) && after_party == changed_party, "Rollback did not restore changed native party state");
        const auto rollback_writes = exi_memcard_writes();
        const auto rollback_disk = disk_card();
        const auto rollback_tb = ssne_timebase();
        log_message("[transaction-rollback-observed] mode=" + std::to_string(mode) + " writes=" + std::to_string(rollback_writes) + " expected_writes=" + std::to_string(card_writes) + " disk_equal=" + std::to_string(rollback_disk == original_disk) + " clock=" + std::to_string(rollback_tb) + " expected_clock=" + std::to_string(frozen_tb));
        require(rollback_writes == card_writes && rollback_disk == original_disk && rollback_tb == frozen_tb, "Rollback changed backing card or clock");
        ++rollback_cases;
    }
    ctx.r[3] ^= 0x12345678u; g_ram[0x4000] ^= 0xA5; g_locked_cache[0] ^= 0x5A;
    require(load_transaction(ctx, data, party, native_party_save_state, native_party_load_state, why), "Valid combined checkpoint failed");
    std::vector<uint8_t> restored_party;
    require(native_party_save_state(restored_party, why) && restored_party == party, "Successful combined load did not restore native party");
    require(!memcmp(&ctx, &original_ctx, sizeof(ctx)) && !memcmp(g_ram, ram.data(), ram_size) && !memcmp(g_locked_cache, cache.data(), cache.size()), "Combined checkpoint did not restore runtime");
    require(load_transaction(ctx, changed_card_target, party, native_party_save_state, native_party_load_state, why), "Card commit positive control failed");
    require(disk_card() == exi_memcard_snapshot() && disk_card() != original_disk && exi_memcard_writes() == card_writes+1, "Committed card was not persisted exactly once");
    require(load_transaction(ctx, data, party, native_party_save_state, native_party_load_state, why), "Original card restoration failed");
    log_message("[transaction-card-after] disk_original=" + std::to_string(disk_card() == original_disk) + " disk_snapshot=" + std::to_string(disk_card() == exi_memcard_snapshot_before_transaction) + " writes=" + std::to_string(exi_memcard_writes()));
    require(disk_card() == exi_memcard_snapshot_before_transaction && exi_memcard_writes() == card_writes+2, "Saved card was not restored exactly once");
    {
        std::ofstream transaction("runtime-transaction-probe.json");
        transaction << "{\"rollback_cases\":" << rollback_cases << ",\"post_ram_binding_rejected\":true,\"post_participant_exception_rolled_back\":true,\"card_commit_failure_rolled_back\":true,\"cpu_ram_cache_modules_party_clock_restored\":true,\"backing_card_unchanged_on_failure\":true,\"changed_party_restored\":true,\"card_commit_and_restore_writes\":2,\"valid_load_card_writes\":" << valid_load_writes << ",\"dirty_card_fixture\":" << (getenv("SSNE_TEST_CHECKPOINT_DIRTY_CARD") ? "true" : "false") << ",\"valid_combined_restore\":true,\"players\":" << players << "}\n";
        require(bool(transaction), "Cannot write transaction evidence");
    }
#ifdef _WIN32
    {
        const std::string lock_path="runtime-checkpoint-sharing.state";
        const std::vector<uint8_t> old_bytes{1,2,3,4},new_bytes{5,6,7,8};
        require(write_file(lock_path,old_bytes,why),"Cannot create sharing test file");
        auto open_lock=[&]{return CreateFileW(std::filesystem::u8path(lock_path).c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);};
        auto lock=open_lock();require(lock!=INVALID_HANDLE_VALUE,"Cannot acquire save replacement test lock");
        unsigned rejected_retries=0;const bool locked_write=write_file(lock_path,new_bytes,why,&rejected_retries);CloseHandle(lock);
        require(!locked_write && rejected_retries==20 && why.find("Windows error")!=std::string::npos,"Locked save replacement did not fail safely");
        auto disk=[&]{std::ifstream in(lock_path,std::ios::binary);return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),{});};
        require(disk()==old_bytes,"Failed replacement destroyed the old save");
        lock=open_lock();require(lock!=INVALID_HANDLE_VALUE,"Cannot reacquire save replacement test lock");
        std::thread release([lock]{std::this_thread::sleep_for(std::chrono::milliseconds(50));CloseHandle(lock);});
        unsigned successful_retries=0;bool replaced=false;
        try{replaced=write_file(lock_path,new_bytes,why,&successful_retries);}catch(...){release.join();throw;}
        release.join();
        require(replaced && successful_retries>0 && disk()==new_bytes,"Save replacement did not recover after sharing lock release");
        std::ofstream evidence("runtime-sharing-probe.json");evidence<<"{\"locked_retries\":"<<rejected_retries<<",\"released_retries\":"<<successful_retries<<",\"old_save_preserved\":true,\"new_save_committed\":true}\n";
        require(bool(evidence),"Cannot record save sharing evidence");
    }
#endif
    resume_tb = ssne_timebase(); resume_vi = scheduler_stats().retraces; resume_ai = audio_stats().ai_frames; resume_generation = frozen_generation;
    resume_started = std::chrono::steady_clock::now(); await_resume = true;
    {
        std::ofstream quiescence("runtime-quiescence-probe.json");
        quiescence << "{\"frozen_ms\":80,\"clock_ticks_changed\":0,\"retraces_changed\":0,\"audio_frames_changed\":0,\"module_bytes_equal\":true,\"nested_generation_unchanged\":true,\"audio_frames_before\":" << frozen_ai << ",\"retraces_before\":" << frozen_vi << "}\n";
        require(bool(quiescence), "Cannot write checkpoint barrier evidence");
    }
    std::ofstream audit("runtime-checkpoint-probe.json");
    audit << "{\"rejected\":" << rejected << ",\"ram_bytes\":" << ram_size << ",\"players\":" << players << ",\"registry\":" << registry << ",\"restored\":true,\"card_unchanged_on_rejection\":true,\"scope\":\"Runtime file layout preflight and same-frame CPU/RAM/cache restoration; whole network-session rewind NOT VERIFIED\"}\n";
    require(bool(audit), "Cannot write runtime probe evidence");
    log_message("[runtime-checkpoint-probe] rejected=" + std::to_string(rejected) + " restored=1 ram=" + std::to_string(ram_size));
}
