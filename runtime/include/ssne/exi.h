// EXI bus + memory card (slot A) emulation.
#pragma once
#include <cstdint>
#include <vector>
#include <string>

namespace ssne {

// Before boot: memory card image for slot A (created blank if missing; empty path = no card inserted).
// Size is in Mbit for new cards: 4 (59 blocks), 8 (123), 16 (251), 32 (507), 64 (1019), 128 (2043).
void exi_configure_memcard(const std::string& path_a, uint32_t size_mbit = 16);
// Mount local profiles/settings in RAM only. Campaign saves remain host-owned.
bool exi_configure_volatile_memcard(const std::string& seed_path, uint32_t size_mbit=16);
std::vector<uint8_t> exi_memcard_snapshot();
bool exi_configure_memcard_snapshot(const std::vector<uint8_t>& data);
std::string exi_memcard_path();   // image in use ("" if none)
uint64_t exi_memcard_writes();    // number of write-throughs to the image
// Guest safe point only: isolate an in-process client and restore its local card.
bool exi_begin_client_session();
void exi_end_client_session();

uint16_t exi_read16(uint32_t off);
void exi_write16(uint32_t off, uint16_t v);

} // namespace ssne
