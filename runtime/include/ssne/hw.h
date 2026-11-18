// Hardware register emulation for the blocks the SDK drivers poke directly.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ssne {

// Disc contents are served from the extracted game folder (sys/ + files/), mapped back to disc offsets
// through the FST, so the SDK's DVD driver reads "the disc" unmodified.
bool disc_open(const std::string& data_dir, std::string& error);
bool disc_read(uint64_t offset, uint8_t* dst, uint32_t len);
// Add immutable data beyond all existing virtual extents. Existing file
// offsets and outstanding DVDFileInfo reads retain their original contents.
uint64_t disc_append_offset();
bool disc_append_memory(uint64_t offset,
                        std::shared_ptr<const std::vector<uint8_t>> bytes,
                        std::string& error);

void vi_retrace();                 // called at 60 Hz by the scheduler's timer
uint32_t vi_framebuffer_address(); // current XFB (top field) physical address, for display
uint8_t* aram();                   // 16 MB auxiliary RAM

struct HwStats { uint64_t aram_dmas, disc_reads, disc_bytes; };
HwStats hw_stats();

} // namespace ssne
