// EXI (0xCC006800): the external interface bus, with a GameCube memory card in slot A.
//
// The SDK's EXI and CARD libraries run unmodified: they select the card, send flash commands (read array,
// sector erase, page program, status...) with immediate transfers, and move sector data with DMA. The card
// is backed by a raw image file with the same layout as a real card (and Dolphin's .raw cards), written
// through as the game erases/programs it. Other EXI devices (IPL/SRAM/RTC, slot B, serial port 1) stay
// absent: their transfers complete instantly, as before.
//
// Register layout per channel (stride 0x14): +0 CSR, +4 DMA address, +8 DMA length, +0C control, +10 data.
// CSR bits: 0 EXIINTMASK, 1 EXIINT, 2 TCINTMASK, 3 TCINT, 4-6 CLK, 7-9 CS, 10 EXTINTMASK, 11 EXTINT, 12 EXT,
//           13 ROMDIS. Control: 0 TSTART, 1 DMA, 2-3 RW (0 read, 1 write, 2 read/write), 4-5 TLEN (bytes - 1).
// The device behavior follows the reverse-engineered description of the card's flash protocol (Dolphin's
// EXI_DeviceMemoryCard was used as the reference).
#include "ssne/exi.h"
#include "state_io.h"

#include <ctime>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>
#include <thread>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

#include "ssne/ppc_context.h"
#include "ssne/scheduler.h"

namespace fs = std::filesystem;

namespace ssne {

namespace {

constexpr uint32_t CSR_EXIINTMASK = 1u << 0, CSR_EXIINT = 1u << 1, CSR_TCINTMASK = 1u << 2, CSR_TCINT = 1u << 3,
                   CSR_CLK = 7u << 4, CSR_CS = 7u << 7, CSR_EXTINTMASK = 1u << 10, CSR_EXTINT = 1u << 11,
                   CSR_EXT = 1u << 12, CSR_ROMDIS = 1u << 13;

constexpr uint8_t MC_BUSY = 0x80, MC_UNLOCKED = 0x40, MC_ERASEERROR = 0x10, MC_PROGERROR = 0x08, MC_READY = 0x01;
constexpr uint32_t kSector = 0x2000;  // 8 KB erase block

bool g_trace = getenv("SSNE_TRACE_EXI") != nullptr;

// ------------------------------------------------------------------ memory card
struct MemCard {
    std::string path;
    std::vector<uint8_t> data;
    uint32_t size_mbit = 16;
    // flash state
    uint8_t command = 0;
    uint32_t position = 0;
    uint32_t address = 0;
    uint8_t status = MC_BUSY | MC_UNLOCKED | MC_READY;
    uint8_t interrupt_switch = 0;
    bool interrupt_set = false;
    uint8_t prog[128] = {};
    uint64_t writes = 0;
    // Backing-file failures must not disappear when another page is written.
    // Keep the outstanding range until an actual write/close succeeds. This is
    // host I/O state, not emulated flash state, and is not rewound by savestates.
    uint32_t dirty_begin = 0, dirty_end = 0;
    bool test_lock_injected = false;
    uint32_t test_lock_matches = 0;

    void inject_test_lock(uint32_t off,uint32_t len) {
#ifdef _WIN32
        // Hidden fixture only: acquire a real OS sharing lock immediately
        // before a selected native flash write. Do not simulate error returns.
        if(test_lock_injected || !getenv("SSNE_TEST_HIDDEN") || !getenv("SSNE_COOP_TEST_CARD_OBSERVE"))return;
        const auto offset_text=getenv("SSNE_TEST_CARD_LOCK_OFFSET"),length_text=getenv("SSNE_TEST_CARD_LOCK_BYTES"),duration_text=getenv("SSNE_TEST_CARD_LOCK_MS");
        if(!offset_text || !length_text || !duration_text)return;
        const auto offset=strtoul(offset_text,nullptr,0),length=strtoul(length_text,nullptr,0),duration=strtoul(duration_text,nullptr,0);
        if(off!=offset || len!=length || !duration || duration>10000)return;
        const auto occurrence_text=getenv("SSNE_TEST_CARD_LOCK_OCCURRENCE");
        const auto occurrence=occurrence_text?strtoul(occurrence_text,nullptr,0):1ul;
        if(!occurrence || occurrence>100 || ++test_lock_matches!=occurrence)return;
        test_lock_injected=true;
        const auto lock=CreateFileW(fs::u8path(path).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(lock==INVALID_HANDLE_VALUE){fprintf(stderr,"[exi-test-lock] acquire-failed win32=%lu\n",GetLastError());return;}
        const auto started=GetTickCount64();
        fprintf(stdout,"[exi-test-lock] acquired offset=%u bytes=%u writes=%llu requested_ms=%lu clock_ms=%llu occurrence=%lu\n",off,len,(unsigned long long)writes,duration,(unsigned long long)started,occurrence);fflush(stdout);
        std::thread([lock,started,duration]{
            Sleep(duration);
            const bool released=CloseHandle(lock)!=0;
            fprintf(stdout,"[exi-test-lock] released ok=%u elapsed_ms=%llu\n",unsigned(released),(unsigned long long)(GetTickCount64()-started));fflush(stdout);
        }).detach();
#endif
    }

    uint32_t mask() const { return uint32_t(data.size()) - 1; }

    bool flush(uint32_t off, uint32_t len) {  // write-through to the image file
        writes++;
        if (path.empty()) return true;
        inject_test_lock(off,len);
        if (len) {
            dirty_begin = dirty_end ? std::min(dirty_begin, off) : off;
            dirty_end = std::max(dirty_end, off + len);
        }
        if (!dirty_end) return true;
#ifdef _WIN32
        // Indexers/sync clients can hold a short sharing lock between flash
        // pages. Retry that host-side condition before reporting a failed
        // emulated flash operation. Persistent failures still retain the dirty
        // range and reach the guest; never claim a write that did not happen.
        const auto native_path=fs::u8path(path);
        HANDLE file=INVALID_HANDLE_VALUE;
        DWORD failure=ERROR_SUCCESS;
        unsigned retries=0;
        for (;;) {
            file=CreateFileW(native_path.c_str(),GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,nullptr,
                OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(file!=INVALID_HANDLE_VALUE)break;
            failure=GetLastError();
            if((failure!=ERROR_SHARING_VIOLATION && failure!=ERROR_LOCK_VIOLATION) || retries==20) {
                fprintf(stderr,"[exi] card backing open failed: offset=%u bytes=%u win32=%lu retries=%u path=%s\n",dirty_begin,dirty_end-dirty_begin,failure,retries,path.c_str());
                return false;
            }
            ++retries;Sleep(5);
        }
        LARGE_INTEGER offset{};offset.QuadPart=dirty_begin;
        DWORD written=0;
        bool ok=SetFilePointerEx(file,offset,nullptr,FILE_BEGIN)!=0;
        if(ok)ok=WriteFile(file,data.data()+dirty_begin,dirty_end-dirty_begin,&written,nullptr)!=0;
        if(!ok)failure=GetLastError();
        else if(written!=dirty_end-dirty_begin){ok=false;failure=ERROR_WRITE_FAULT;}
        if(!CloseHandle(file)){ok=false;failure=GetLastError();}
        if(!ok){fprintf(stderr,"[exi] card backing write failed: offset=%u bytes=%u win32=%lu path=%s\n",dirty_begin,dirty_end-dirty_begin,failure,path.c_str());return false;}
        if(retries)fprintf(stdout,"[exi] card backing sharing retry recovered: offset=%u bytes=%u retries=%u\n",dirty_begin,dirty_end-dirty_begin,retries);
#else
        std::fstream f(fs::u8path(path), std::ios::in | std::ios::out | std::ios::binary);
        if (!f) { fprintf(stderr,"[exi] card backing open failed: offset=%u bytes=%u path=%s\n",dirty_begin,dirty_end-dirty_begin,path.c_str()); return false; }
        f.seekp(dirty_begin);
        f.write(reinterpret_cast<const char*>(data.data() + dirty_begin), dirty_end-dirty_begin);
        f.flush();f.close();
        if (!f) { fprintf(stderr,"[exi] card backing write failed: offset=%u bytes=%u path=%s\n",dirty_begin,dirty_end-dirty_begin,path.c_str()); return false; }
#endif
        dirty_begin=dirty_end=0;
        return true;
    }
    void read(uint32_t addr, uint32_t len, uint8_t* dst) const {
        for (uint32_t i = 0; i < len; i++) dst[i] = data[(addr + i) & mask()];
    }
    void write(uint32_t addr, uint32_t len, const uint8_t* src) {
        addr &= mask();
        len = std::min<uint32_t>(len, uint32_t(data.size()) - addr);
        memcpy(data.data() + addr, src, len);
        if (!flush(addr, len)) status |= MC_PROGERROR;
    }

    // Called when the chip select goes low (end of a command): erase/program take effect.
    // Returns true if the card raises its "command done" interrupt.
    bool deselect() {
        switch (command) {
        case 0xF1:  // sector erase
            if (position > 2) {
                uint32_t a = address & mask() & ~(kSector - 1);
                memset(data.data() + a, 0xFF, kSector);
                if (!flush(a, kSector)) status |= MC_ERASEERROR;
                status = uint8_t((status | MC_READY) & ~MC_BUSY);
                interrupt_set = true;
                return true;
            }
            break;
        case 0xF4:  // chip erase
            if (position > 2) {
                std::fill(data.begin(), data.end(), 0xFF);
                if (!flush(0, uint32_t(data.size()))) status |= MC_ERASEERROR;
                status &= ~MC_BUSY;
            }
            break;
        case 0xF2:  // page program (data came either byte-wise here or by DMA)
            if (position >= 5) {
                uint32_t count = position - 5, i = 0;
                uint32_t begin=uint32_t(data.size()),end=0;
                while (count--) {
                    const auto offset=address & mask();
                    data[offset]=prog[i++];
                    begin=std::min(begin,offset);end=std::max(end,offset+1);
                    i &= 127;
                    address = (address & ~0x1FFu) | ((address + 1) & 0x1FF);
                }
                // One completed flash command gets one backing write/retry
                // budget, including immediate transfers that wrap a page.
                if(end>begin && !flush(begin,end-begin))status |= MC_PROGERROR;
                status = uint8_t((status | MC_READY) & ~MC_BUSY);
                interrupt_set = true;
                return true;
            }
            break;
        default: break;
        }
        return false;
    }

    void transfer_byte(uint8_t& b) {
        if (position == 0) {
            command = b;
            b = 0xFF;
            if (command == 0x89) {  // clear status
                status &= ~(MC_PROGERROR | MC_ERASEERROR);
                status |= MC_READY;
                interrupt_set = false;
                position = 0;
                return;  // Dolphin keeps position at 0 for this one-byte command
            }
        } else {
            switch (command) {
            case 0x00:  // Nintendo EXI ID: card size in Mbit
                b = position == 1 ? 0x80 : uint8_t(size_mbit >> (24 - (((position - 2) & 3) * 8)));
                break;
            case 0x52:  // read array: AD1 AD2 AD3 BA, 4 dummy bytes, then data
                switch (position) {
                case 1: address = uint32_t(b) << 17; b = 0xFF; break;
                case 2: address |= uint32_t(b) << 9; break;
                case 3: address |= uint32_t(b & 3) << 7; break;
                case 4: address |= b & 0x7F; break;
                }
                if (position > 1) {
                    read(address, 1, &b);
                    if (position >= 9) address = (address & ~0x1FFu) | ((address + 1) & 0x1FF);
                }
                break;
            case 0x83: b = status; break;  // read status
            case 0x85: {                    // read ID (0xC221: Nintendo-brand card)
                const uint16_t id = 0xC221;
                b = position == 1 ? uint8_t(id >> 8) : uint8_t((position & 1) ? id : id >> 8);
                break;
            }
            case 0xF1:  // sector erase: AD1 AD2
                if (position == 1) address = uint32_t(b) << 17;
                else if (position == 2) address |= uint32_t(b) << 9;
                b = 0xFF;
                break;
            case 0x81:  // set interrupt
                if (position == 1) interrupt_switch = b;
                b = 0xFF;
                break;
            case 0xF4: b = 0xFF; break;
            case 0xF2:  // page program: AD1 AD2 AD3 BA, then up to 128 data bytes
                switch (position) {
                case 1: address = uint32_t(b) << 17; break;
                case 2: address |= uint32_t(b) << 9; break;
                case 3: address |= uint32_t(b & 3) << 7; break;
                case 4: address |= b & 0x7F; break;
                }
                if (position >= 5) prog[(position - 5) & 0x7F] = b;
                b = 0xFF;
                break;
            default:
                if (g_trace) fprintf(stdout, "[exi] memcard: unknown command %02X byte %u\n", command, position);
                b = 0xFF;
                break;
            }
        }
        position++;
    }

    bool interrupt() const { return interrupt_switch && interrupt_set; }
};

// ------------------------------------------------------------------ IPL device (channel 0, chip select 2)
// Addresses (command >> 6): 0x800000 = RTC (4 bytes) followed by SRAM (64 bytes). ROM reads return zeros
// (no IPL ROM is used). Command: 4 bytes, bit 31 = write.
struct Ipl {
    uint8_t mem[0x44] = {};   // RTC + SRAM
    uint32_t cmd = 0, cmd_bytes = 0, cursor = 0;

    static uint32_t rtc_now() {  // seconds since 2000-01-01, local time
        std::time_t t = std::time(nullptr);
        std::tm lt{}, gt{};
#ifdef _WIN32
        localtime_s(&lt, &t); gmtime_s(&gt, &t);
#else
        localtime_r(&t, &lt); gmtime_r(&t, &gt);
#endif
        gt.tm_isdst = -1;
        long off = long(std::mktime(&lt) - std::mktime(&gt));
        return uint32_t(int64_t(t) + off - 946684800);
    }
    void init() {
        // SRAM defaults (like a console's): English, stereo, no flash IDs recorded yet.
        uint8_t* s = mem + 4;
        memset(s, 0, 64);
        s[0x10] = 0;     // displayOffsetH
        s[0x11] = 0;     // ntd
        s[0x12] = 0;     // language: English
        s[0x13] = 0x2C;  // flags: stereo (bit 2), + the bits a configured console has
        s[0x3A] = 0xFF;  // flashIDCheckSum[0] = ~sum(flashID[0]) (all zero)
        s[0x3B] = 0xFF;  // flashIDCheckSum[1]
        fix_checksum();
    }
    void fix_checksum() {
        uint8_t* s = mem + 4;
        uint16_t cs = 0, csi = 0;
        for (int i = 0; i < 4; i++) {
            uint16_t w = uint16_t(s[0x0C + i * 2] << 8 | s[0x0D + i * 2]);
            cs = uint16_t(cs + w);
            csi = uint16_t(csi + uint16_t(~w));
        }
        s[0] = uint8_t(cs >> 8); s[1] = uint8_t(cs);
        s[2] = uint8_t(csi >> 8); s[3] = uint8_t(csi);
    }
    void select() { cmd = 0; cmd_bytes = 0; cursor = 0; }
    void transfer_byte(uint8_t& b) {
        if (cmd_bytes < 4) {
            cmd = cmd << 8 | b;
            b = 0xFF;
            if (++cmd_bytes == 4) {
                uint32_t rtc = rtc_now();
                mem[0] = uint8_t(rtc >> 24); mem[1] = uint8_t(rtc >> 16); mem[2] = uint8_t(rtc >> 8); mem[3] = uint8_t(rtc);
            }
            return;
        }
        uint32_t addr = (cmd >> 6) & 0x1FFFFFF;
        bool wr = cmd >> 31;
        if (addr >= 0x800000 && addr < 0x800000 + sizeof(mem)) {
            uint32_t a = (addr - 0x800000 + cursor++) % sizeof(mem);
            if (wr) mem[a] = b; else b = mem[a];
        } else if (!wr) {
            b = 0;
        }
    }
    const uint8_t* sram() const { return mem + 4; }
};
Ipl g_ipl;

// Big-endian word checksum used by the CARD library (__CARDCheckSum).
void card_checksum(const uint8_t* p, uint32_t len, uint16_t& cs, uint16_t& csi) {
    cs = 0; csi = 0;
    for (uint32_t i = 0; i + 1 < len; i += 2) {
        uint16_t w = uint16_t(p[i] << 8 | p[i + 1]);
        cs = uint16_t(cs + w);
        csi = uint16_t(csi + uint16_t(~w));
    }
    if (cs == 0xFFFF) cs = 0;
    if (csi == 0xFFFF) csi = 0;
}
inline void put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
inline void put32(uint8_t* p, uint32_t v) { put16(p, uint16_t(v >> 16)); put16(p + 2, uint16_t(v)); }

// Diagnostic only: retain the SDK's source directory and the assembled flash
// directory at page boundaries. This distinguishes a changing SDK buffer from
// backing-file failures; never repair checksums or alter guest/card bytes.
void trace_directory_dma(const MemCard& card, uint32_t mar, uint32_t len) {
    static const bool enabled = getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TEST_CARD_OBSERVE");
    if (!enabled || card.path.empty()) return;
    const auto address = card.address & card.mask();
    if (address < 0x2000 || address >= 0x6000) return;
    const auto offset = address & 0x1FFFu, sector = address & ~0x1FFFu;
    const auto physical = mar & (kRamSize - 1);
    if (physical < offset || physical - offset > kRamSize - kSector) return;
    const auto* source = g_ram + physical - offset;
    const auto* assembled = card.data.data() + sector;
    uint16_t source_sum, source_inverse, card_sum, card_inverse;
    card_checksum(source, 0x1FFC, source_sum, source_inverse);
    card_checksum(assembled, 0x1FFC, card_sum, card_inverse);
    auto word = [](const uint8_t* p) { return unsigned(p[0]) * 256 + p[1]; };
    fprintf(stdout, "[exi-directory-dma] writes=%llu address=%u bytes=%u source=%u source_counter=%u source_sum=%u source_inverse=%u source_stored=%u source_stored_inverse=%u card_counter=%u card_sum=%u card_inverse=%u card_stored=%u card_stored_inverse=%u status=%u\n",
        (unsigned long long)card.writes, address, len, physical - offset,
        word(source+0x1FFA), source_sum, source_inverse, word(source+0x1FFC), word(source+0x1FFE),
        word(assembled+0x1FFA), card_sum, card_inverse, word(assembled+0x1FFC), word(assembled+0x1FFE), card.status);
    if (!offset || offset + len >= kSector) {
        const auto prefix = card.path + ".directory-" + std::to_string(card.writes);
        for (bool guest : {true, false}) {
            std::ofstream f(fs::u8path(prefix + (guest ? "-source.bin" : "-flash.bin")), std::ios::binary);
            f.write(reinterpret_cast<const char*>(guest ? source : assembled), kSector);
            if (!f) fprintf(stderr, "[exi] directory diagnostic write failed\n");
        }
    }
}

// Format a card image the way the SDK's CARDFormat does (header, 2 directories, 2 block allocation tables),
// with a serial number that matches this console's SRAM flash ID for the slot, so CARDMount accepts it.
void format_card(std::vector<uint8_t>& d, uint32_t size_mbit, int slot, const Ipl& ipl) {
    std::fill(d.begin(), d.end(), 0xFF);
    uint8_t* id = d.data();
    const uint8_t* sram = ipl.sram();
    // serial[20..23] = counterBias, serial[24..27] = language, serial[28..31] = VI DTV status
    memcpy(id + 20, sram + 0x0C, 4);
    put32(id + 24, sram[0x12]);
    put32(id + 28, 0);
    uint64_t time = uint64_t(Ipl::rtc_now()) * 40500000ull;  // OSTime (timebase ticks)
    uint64_t rnd = time;
    const uint8_t* flash = sram + 0x14 + 12 * slot;
    for (int i = 0; i < 12; i++) {
        rnd = (rnd * 1103515245 + 12345) >> 16;
        id[i] = uint8_t(flash[i] + rnd);
        rnd = ((rnd * 1103515245 + 12345) >> 16) & 0x7FFF;
    }
    for (int i = 0; i < 8; i++) id[12 + i] = uint8_t(time >> (56 - 8 * i));
    put16(id + 0x20, 0);                  // deviceID
    put16(id + 0x22, uint16_t(size_mbit));
    put16(id + 0x24, 0);                  // encode: ANSI (USA)
    uint16_t cs, csi;
    card_checksum(id, 0x1FC, cs, csi);
    put16(id + 0x1FC, cs); put16(id + 0x1FE, csi);
    for (int i = 0; i < 2; i++) {  // directory blocks 1, 2 (empty entries are 0xFF)
        uint8_t* dir = d.data() + (1 + i) * kSector;
        put16(dir + 0x1FFA, uint16_t(i));  // update counter
        card_checksum(dir, 0x1FFC, cs, csi);
        put16(dir + 0x1FFC, cs); put16(dir + 0x1FFE, csi);
    }
    uint32_t blocks = uint32_t(d.size() / kSector);
    for (int i = 0; i < 2; i++) {  // block allocation tables 3, 4
        uint8_t* fat = d.data() + (3 + i) * kSector;
        memset(fat, 0, kSector);
        put16(fat + 4, uint16_t(i));           // update counter
        put16(fat + 6, uint16_t(blocks - 5));  // free blocks
        put16(fat + 8, 4);                     // last allocated
        card_checksum(fat + 4, 0x1FFC, cs, csi);
        put16(fat + 0, cs); put16(fat + 2, csi);
    }
}

// Record the inserted card's flash ID in SRAM (derived from its header serial), as a console does after
// unlocking a card. This lets cards formatted elsewhere (Dolphin .raw images, dumps of real cards) mount.
void set_flash_id(Ipl& ipl, int slot, const uint8_t* hdr) {
    uint64_t rnd = 0;
    for (int i = 0; i < 8; i++) rnd = rnd << 8 | hdr[12 + i];
    uint8_t* flash = ipl.mem + 4 + 0x14 + 12 * slot;
    uint8_t sum = 0;
    for (int i = 0; i < 12; i++) {
        rnd = (rnd * 1103515245 + 12345) >> 16;
        flash[i] = uint8_t(hdr[i] - uint8_t(rnd));
        sum = uint8_t(sum + flash[i]);
        rnd = ((rnd * 1103515245 + 12345) >> 16) & 0x7FFF;
    }
    ipl.mem[4 + 0x3A + slot] = uint8_t(sum ^ 0xFF);
}

// ------------------------------------------------------------------ channels
struct Channel {
    uint32_t csr = 0, mar = 0, len = 0, cr = 0, data = 0;
    uint16_t lo[5] = {}, hi[5] = {};  // raw halfwords as written (for 32-bit assembly)
    bool level_exi = false, level_tc = false, level_ext = false;
};

std::mutex g_mutex;
Channel g_ch[3];
MemCard* g_card[2] = {nullptr, nullptr};  // slot A = channel 0, slot B = channel 1
std::string g_card_path_a = "saves/MemoryCardA.USA.raw";
uint32_t g_card_mbit = 16;
bool g_card_enabled = true;
bool g_initialized = false;
std::vector<uint8_t> g_card_snapshot;
std::vector<uint8_t> g_client_card_before;
std::string g_client_card_path;
bool g_client_card_leased=false;
uint64_t g_card_ops = 0;

MemCard* card_on(int ch) {  // the card is device 0 (chip select 1)
    if (ch > 1 || !g_card[ch]) return nullptr;
    return ((g_ch[ch].csr & CSR_CS) >> 7) == 1 ? g_card[ch] : nullptr;
}

bool load_card(MemCard& c, const std::string& path, uint32_t mbit) {
    c.path = path;
    c.size_mbit = mbit;
    std::error_code ec;
    fs::path p = fs::u8path(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    uint64_t want = uint64_t(mbit) * 1024 * 1024 / 8;
    if (fs::exists(p, ec)) {
        uint64_t sz = fs::file_size(p, ec);
        // Accept any standard card size (4..128 Mbit); the image's size wins.
        if (!ec && sz >= 0x80000 && sz <= 0x1000000 && (sz & (sz - 1)) == 0) {
            c.size_mbit = uint32_t(sz * 8 / (1024 * 1024));
            c.data.resize(size_t(sz));
            std::ifstream in(p, std::ios::binary);
            in.read(reinterpret_cast<char*>(c.data.data()), std::streamsize(sz));
            if (std::all_of(c.data.begin(), c.data.begin() + kSector, [](uint8_t b) { return b == 0xFF; })) {
                format_card(c.data, c.size_mbit, 0, g_ipl);  // blank image: format it
                if (!c.flush(0, uint32_t(c.data.size()))) return false;
                fprintf(stdout, "[exi] formatted blank memory card image\n");
            }
            fprintf(stdout, "[exi] memory card A: %s (%u Mbit, %u blocks)\n", path.c_str(), c.size_mbit,
                    c.size_mbit * 16 - 5);
            return true;
        }
        fprintf(stdout, "[exi] memory card image %s has an unexpected size; leaving it alone\n", path.c_str());
        return false;
    }
    // New card: formatted for this console (a blank card would read as "damaged" to the game).
    c.data.assign(size_t(want), 0xFF);
    format_card(c.data, mbit, 0, g_ipl);
    std::ofstream out(p, std::ios::binary);
    if (!out) { fprintf(stdout, "[exi] can't create memory card image %s\n", path.c_str()); return false; }
    out.write(reinterpret_cast<const char*>(c.data.data()), std::streamsize(want));
    fprintf(stdout, "[exi] created memory card A: %s (%u Mbit, %u blocks)\n", path.c_str(), mbit, mbit * 16 - 5);
    return true;
}

void init_locked() {
    if (g_initialized) return;
    g_initialized = true;
    g_ipl.init();
    g_ch[0].csr |= CSR_EXTINT;  // devices "inserted" at power on
    g_ch[1].csr |= CSR_EXTINT;
    if (!g_card_snapshot.empty()) {
        auto* c = new MemCard;
        c->data = std::move(g_card_snapshot);
        c->size_mbit = uint32_t(c->data.size() * 8 / (1024 * 1024));
        g_card[0] = c;
        set_flash_id(g_ipl, 0, c->data.data());
        return;
    }
    if (g_card_enabled && !g_card_path_a.empty()) {
        auto* c = new MemCard;
        if (load_card(*c, g_card_path_a, g_card_mbit)) { g_card[0] = c; set_flash_id(g_ipl, 0, c->data.data()); }
        else delete c;
    }
}

uint32_t csr_value(int ch) {
    Channel& c = g_ch[ch];
    uint32_t v = c.csr & ~CSR_EXT;
    if (ch < 2 && g_card[ch]) {
        v |= CSR_EXT;
        if (g_card[ch]->interrupt()) v |= CSR_EXIINT;
    }
    return v;
}

// Raise OS interrupts on rising edges of (status & mask). OS interrupt numbers: EXI_n_EXI = 9 + 3n,
// EXI_n_TC = 10 + 3n, EXI_n_EXT = 11 + 3n.
void update_interrupts(int ch) {
    Channel& c = g_ch[ch];
    uint32_t v = csr_value(ch);
    if (v & CSR_EXIINT) c.csr |= CSR_EXIINT;  // latch the device interrupt
    v = csr_value(ch) | (c.csr & CSR_EXIINT);
    bool exi = (v & CSR_EXIINT) && (v & CSR_EXIINTMASK);
    bool tc = (v & CSR_TCINT) && (v & CSR_TCINTMASK);
    bool ext = ch < 2 && (v & CSR_EXTINT) && (v & CSR_EXTINTMASK);
    if (exi && !c.level_exi) raise_interrupt(9 + 3 * ch);
    if (tc && !c.level_tc) raise_interrupt(10 + 3 * ch);
    if (ext && !c.level_ext) raise_interrupt(11 + 3 * ch);
    c.level_exi = exi; c.level_tc = tc; c.level_ext = ext;
}

void write_csr(int ch, uint32_t v) {
    Channel& c = g_ch[ch];
    uint32_t old_cs = (c.csr & CSR_CS) >> 7;
    uint32_t n = c.csr;
    n = (n & ~CSR_EXIINTMASK) | (v & CSR_EXIINTMASK);
    if (v & CSR_EXIINT) {
        n &= ~CSR_EXIINT;
        // Acknowledging the EXI interrupt doesn't clear the card's own flag (ClearStatus does); the
        // latched copy is what's cleared here.
    }
    n = (n & ~CSR_TCINTMASK) | (v & CSR_TCINTMASK);
    if (v & CSR_TCINT) n &= ~CSR_TCINT;
    n = (n & ~CSR_CLK) | (v & CSR_CLK);
    if (ch < 2) {
        n = (n & ~CSR_EXTINTMASK) | (v & CSR_EXTINTMASK);
        if (v & CSR_EXTINT) n &= ~CSR_EXTINT;
    }
    if (ch == 0) n = (n & ~CSR_ROMDIS) | (v & CSR_ROMDIS);
    uint32_t new_cs = (v & CSR_CS) >> 7;
    n = (n & ~CSR_CS) | (v & CSR_CS);
    c.csr = n;
    if (ch == 0 && new_cs == 2 && old_cs != 2) g_ipl.select();
    // Chip select edges for the card (device 0 = CS value 1).
    if (ch < 2 && g_card[ch]) {
        bool was = old_cs == 1, now = new_cs == 1;
        if (!was && now) g_card[ch]->position = 0;
        if (was && !now) g_card[ch]->deselect();
    }
    update_interrupts(ch);
}

void start_transfer(int ch) {
    Channel& c = g_ch[ch];
    MemCard* card = card_on(ch);
    if (ch == 0 && ((c.csr & CSR_CS) >> 7) == 2) {  // IPL: SRAM / RTC (byte-wise device)
        uint32_t tlen = ((c.cr >> 4) & 3) + 1;
        int rw = (c.cr >> 2) & 3;
        if (!(c.cr & 2)) {
            if (rw == 0) {
                uint32_t r = 0;
                for (uint32_t i = 0; i < tlen; i++) { uint8_t b = 0; g_ipl.transfer_byte(b); r |= uint32_t(b) << (24 - i * 8); }
                c.data = r;
            } else if (rw == 1) {
                uint32_t d = c.data;
                for (uint32_t i = 0; i < tlen; i++) { uint8_t b = uint8_t(d >> 24); g_ipl.transfer_byte(b); d <<= 8; }
            }
        } else {
            uint32_t mar = c.mar & 0x03FFFFE0, len = c.len & 0x03FFFFE0;
            uint8_t* m = g_ram + (mar & (kRamSize - 1));
            for (uint32_t i = 0; i < len; i++) {
                uint8_t b = rw == 1 ? m[i] : 0;
                g_ipl.transfer_byte(b);
                if (rw == 0) m[i] = b;
            }
        }
    } else if (card) {
        g_card_ops++;
        uint32_t tlen = ((c.cr >> 4) & 3) + 1;
        int rw = (c.cr >> 2) & 3;
        if (!(c.cr & 2)) {  // immediate
            if (rw == 0) {
                uint32_t r = 0;
                for (uint32_t i = 0; i < tlen; i++) { uint8_t b = 0; card->transfer_byte(b); r |= uint32_t(b) << (24 - i * 8); }
                c.data = r;
            } else if (rw == 1) {
                uint32_t d = c.data;
                for (uint32_t i = 0; i < tlen; i++) { uint8_t b = uint8_t(d >> 24); card->transfer_byte(b); d <<= 8; }
            }
        } else {  // DMA: the command bytes were sent before; move the data block in one go
            uint32_t mar = c.mar & 0x03FFFFE0, len = c.len & 0x03FFFFE0;
            if (rw == 0) card->read(card->address, len, g_ram + (mar & (kRamSize - 1)));
            else if (rw == 1) {
                card->write(card->address, len, g_ram + (mar & (kRamSize - 1)));
                trace_directory_dma(*card, mar, len);
            }
            if (g_trace) fprintf(stdout, "[exi] card DMA %s %05X len %X\n", rw ? "write" : "read", card->address, len);
        }
    }
    // Transfers on absent devices complete instantly with no data (as before).
    c.cr &= ~1u;
    c.csr |= CSR_TCINT;
    update_interrupts(ch);
}

} // namespace

void exi_configure_memcard(const std::string& path_a, uint32_t size_mbit) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_card_enabled = !path_a.empty();
    g_card_path_a = path_a;
    g_card_mbit = size_mbit;
    g_card_snapshot.clear();
}

bool exi_configure_volatile_memcard(const std::string& seed_path,uint32_t size_mbit) {
    if(size_mbit!=4 && size_mbit!=8 && size_mbit!=16 && size_mbit!=32 && size_mbit!=64 && size_mbit!=128)return false;
    std::vector<uint8_t> data;
    if(!seed_path.empty()) {
        std::ifstream input(fs::u8path(seed_path),std::ios::binary|std::ios::ate);
        if(input) {
            auto size=input.tellg();
            if(size<0x80000 || size>0x1000000 || (uint64_t(size)&(uint64_t(size)-1)))return false;
            data.resize(size_t(size));input.seekg(0);
            if(!input.read(reinterpret_cast<char*>(data.data()),std::streamsize(data.size())))return false;
        }
    }
    std::lock_guard<std::mutex> lock(g_mutex);if(g_initialized)return false;
    if(data.empty()) {g_ipl.init();data.assign(size_t(size_mbit)*1024*1024/8,0xFF);format_card(data,size_mbit,0,g_ipl);}
    g_card_path_a.clear();g_card_enabled=false;g_card_snapshot=std::move(data);return true;
}

std::vector<uint8_t> exi_memcard_snapshot() {
    std::lock_guard<std::mutex> lk(g_mutex);
    init_locked();
    return g_card[0] ? g_card[0]->data : std::vector<uint8_t>{};
}

bool exi_configure_memcard_snapshot(const std::vector<uint8_t>& data) {
    const size_t n = data.size();
    if (n && (n < 0x80000 || n > 0x1000000 || (n & (n - 1)))) return false;
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_initialized) return false;
    g_card_path_a.clear();
    g_card_enabled = false;
    g_card_snapshot = data;
    return true;
}

std::string exi_memcard_path() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_card[0] ? g_card[0]->path : std::string();
}
bool exi_begin_client_session(){
    std::lock_guard lock(g_mutex);init_locked();if(g_client_card_leased)return false;
    if(!g_card[0]){g_client_card_leased=true;return true;}
    if(g_card[0]->dirty_end && !g_card[0]->flush(0,0))return false;
    g_client_card_before=g_card[0]->data;g_client_card_path=g_card[0]->path;g_card[0]->path.clear();g_client_card_leased=true;return true;
}
void exi_end_client_session(){
    std::lock_guard lock(g_mutex);if(!g_client_card_leased)return;
    if(g_card[0]){g_card[0]->data=std::move(g_client_card_before);g_card[0]->path=std::move(g_client_card_path);g_card[0]->dirty_begin=g_card[0]->dirty_end=0;}
    g_client_card_leased=false;
}

uint64_t exi_memcard_writes() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_card[0] ? g_card[0]->writes : 0;
}

uint16_t exi_read16(uint32_t off) {
    std::lock_guard<std::mutex> lk(g_mutex);
    init_locked();
    uint32_t rel = off - 0x6800;
    int ch = int(rel / 0x14);
    if (ch > 2) return 0;
    uint32_t r = rel % 0x14;
    Channel& c = g_ch[ch];
    uint32_t v;
    switch (r & ~3u) {
    case 0x00: v = csr_value(ch) | (c.csr & CSR_EXIINT); break;
    case 0x04: v = c.mar; break;
    case 0x08: v = c.len; break;
    case 0x0C: v = c.cr; break;
    default: v = c.data; break;
    }
    return (r & 2) ? uint16_t(v) : uint16_t(v >> 16);
}

void exi_write16(uint32_t off, uint16_t val) {
    std::lock_guard<std::mutex> lk(g_mutex);
    init_locked();
    uint32_t rel = off - 0x6800;
    int ch = int(rel / 0x14);
    if (ch > 2) return;
    uint32_t r = rel % 0x14;
    Channel& c = g_ch[ch];
    int idx = int(r >> 2);
    if (!(r & 2)) { c.hi[idx] = val; return; }  // high half: wait for the low half (32-bit writes are hi, lo)
    c.lo[idx] = val;
    uint32_t v = uint32_t(c.hi[idx]) << 16 | val;
    switch (idx) {
    case 0: write_csr(ch, v); break;
    case 1: c.mar = v; break;
    case 2: c.len = v; break;
    case 3: c.cr = v; if (v & 1) start_transfer(ch); break;
    case 4: c.data = v; break;
    }
}

void exi_state_save(StateWriter& w) {
    std::lock_guard<std::mutex> lk(g_mutex);
    w.tag("EX01");
    w.bytes(g_ch, sizeof(g_ch));
    w.bytes(g_ipl.mem, sizeof(g_ipl.mem)); w.u32(g_ipl.cmd); w.u32(g_ipl.cmd_bytes); w.u32(g_ipl.cursor);
    MemCard* c = g_card[0];
    w.pod(uint8_t(c ? 1 : 0));
    if (c) {
        w.pod(c->command); w.u32(c->position); w.u32(c->address); w.pod(c->status); w.pod(c->interrupt_switch);
        w.pod(uint8_t(c->interrupt_set)); w.bytes(c->prog, sizeof(c->prog));
        w.block(c->data.data(), c->data.size());
    }
}

static bool exi_state_load_impl(StateReader& r, bool apply, bool persist) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!r.tag("EX01")) return false;
    r.bytes(apply ? g_ch : nullptr, sizeof(g_ch));
    r.bytes(apply ? g_ipl.mem : nullptr, sizeof(g_ipl.mem));
    const auto cmd = r.u32(), cmd_bytes = r.u32(), cursor = r.u32();
    if (apply) { g_ipl.cmd = cmd; g_ipl.cmd_bytes = cmd_bytes; g_ipl.cursor = cursor; }
    uint8_t has = 0;
    r.pod(has);
    if (!r.ok || has > 1 || bool(has) != bool(g_card[0])) { r.ok = false; return false; }
    if (!has) return r.ok;
    MemCard tmp;
    uint8_t iset = 0;
    r.pod(tmp.command); tmp.position = r.u32(); tmp.address = r.u32(); r.pod(tmp.status); r.pod(tmp.interrupt_switch);
    r.pod(iset); r.bytes(tmp.prog, sizeof(tmp.prog));
    if (!r.ok || iset > 1) { r.ok = false; return false; }
    uint64_t size = 0;
    {   // peek at the card size (block() checks it)
        StateReader peek = r;
        size = peek.u64();
    }
    if (!r.ok || size < 0x80000 || size > 0x1000000) return false;
    if (apply) tmp.data.resize(size_t(size));
    if (!r.block(apply ? tmp.data.data() : nullptr, size_t(size))) return false;
    MemCard* c = g_card[0];
    if (!c || c->data.size() != size) { r.ok = false; return false; }
    if (!apply) return true;
    // The card goes back to its contents at the time of the state (the game's cached directory must match it).
    // The image as it was before loading is kept next to it.
    if (persist && !c->path.empty()) {
        std::error_code ec;
        fs::path p = fs::u8path(c->path);
        fs::copy_file(p, fs::u8path(c->path + ".before-state-load.bak"), fs::copy_options::overwrite_existing, ec);
    }
    c->command = tmp.command; c->position = tmp.position; c->address = tmp.address; c->status = tmp.status;
    c->interrupt_switch = tmp.interrupt_switch; c->interrupt_set = iset != 0;
    memcpy(c->prog, tmp.prog, sizeof(c->prog));
    if (c->data != tmp.data) { c->data.swap(tmp.data); if (persist && !c->flush(0, uint32_t(c->data.size()))) return false; }
    else if (persist && c->dirty_end && !c->flush(0, uint32_t(c->data.size()))) return false;
    return r.ok;
}

bool exi_state_load(StateReader& r, bool apply) { return exi_state_load_impl(r, apply, true); }
bool exi_state_load_volatile(StateReader& r) { return exi_state_load_impl(r, true, false); }

bool exi_state_commit(const std::vector<uint8_t>& previous, std::string& error) {
    std::lock_guard<std::mutex> lk(g_mutex);
    auto* c = g_card[0];
    if (!c || c->path.empty() || (c->data == previous && !c->dirty_end)) return true;
    const auto path = fs::u8path(c->path), temp = fs::u8path(c->path + ".state-tmp");
    {
        std::ofstream f(temp, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(c->data.data()), std::streamsize(c->data.size()));
        f.flush(); f.close();
        if (!f) { error = "Cannot write checkpoint card image"; return false; }
    }
    std::error_code ec;
    fs::copy_file(path, fs::u8path(c->path + ".before-state-load.bak"), fs::copy_options::overwrite_existing, ec);
    if (ec) { error = "Cannot preserve previous card image"; return false; }
#ifdef _WIN32
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "Cannot replace checkpoint card image"; return false;
    }
#else
    fs::rename(temp, path, ec);
    if (ec) { error = "Cannot replace checkpoint card image"; return false; }
#endif
    ++c->writes;
    c->dirty_begin=c->dirty_end=0;
    return true;
}

} // namespace ssne
