// AX audio microcode HLE + host audio FIFO.
//
// The game's AX library (Dolphin SDK, ucode CRC 0x07F88145: the standard GameCube AX with a low-pass filter in
// the parameter block) sends the DSP a command list every 5 ms. The DSP walks the linked list of voice parameter
// blocks (AXPB) in main RAM, decodes each running voice from ARAM (DSP-ADPCM / PCM8 / PCM16 through the DSP's
// sample accelerator), resamples it, applies the volume envelope and low-pass filter, and mixes it into
// main/auxA/auxB buses. The final main L/R mix is written back to RAM as 160 stereo frames, which the AX library
// then plays with AI DMA. This file does the DSP's part natively.
//
// Behavior follows the reverse-engineered description of the ucode (Dolphin's AX HLE was used as the reference).
#include "ssne/audio.h"
#include "ssne/native_music.h"
#include "ssne/runtime.h"
#include "state_io.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "ssne/hw.h"
#include "ssne/ppc_context.h"

namespace ssne {

namespace {

// Launch-only diagnostic configuration; live music/SFX volume atomics below
// remain dynamic. Do not scan the Windows environment per DSP voice/subframe.
bool music_capture_probe(){static const bool enabled=getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TEST_CAPTURE_MUSIC");return enabled;}

// ------------------------------------------------------------------ guest memory helpers
inline uint16_t rd16(uint32_t a) { uint16_t v; memcpy(&v, g_ram + (a & (kRamSize - 1)), 2); return __builtin_bswap16(v); }
inline uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, g_ram + (a & (kRamSize - 1)), 4); return __builtin_bswap32(v); }
inline void wr16(uint32_t a, uint16_t x) { x = __builtin_bswap16(x); memcpy(g_ram + (a & (kRamSize - 1)), &x, 2); }
inline void wr32(uint32_t a, uint32_t x) { x = __builtin_bswap32(x); memcpy(g_ram + (a & (kRamSize - 1)), &x, 4); }
inline uint8_t aram8(uint32_t a) { return aram()[a & 0x00FFFFFF]; }

inline int16_t clamp16(int64_t v) { return int16_t(std::clamp<int64_t>(v, -32768, 32767)); }

// ------------------------------------------------------------------ AXPB (u16 word offsets)
enum : int {
    PB_NEXT_HI = 0, PB_NEXT_LO = 1, PB_SRC_TYPE = 4, PB_COEF_SEL = 5, PB_MIXER_CTRL = 6, PB_RUNNING = 7, PB_IS_STREAM = 8,
    PB_MIXER = 9,        // 9 x (volume, delta): mainL mainR auxAL auxAR auxBL auxBR auxBS mainS auxAS
    PB_ITD = 27,
    PB_NUM_UPDATES = 34, PB_UPD_HI = 39, PB_UPD_LO = 40,
    PB_DPOP = 41,        // mainL auxAL auxBL mainR auxAR auxBR mainS auxAS auxBS
    PB_VE_CUR = 50, PB_VE_DELTA = 51,
    PB_LOOPING = 55, PB_FORMAT = 56, PB_LOOP_HI = 57, PB_LOOP_LO = 58, PB_END_HI = 59, PB_END_LO = 60,
    PB_CUR_HI = 61, PB_CUR_LO = 62,
    PB_COEFS = 63, PB_GAIN = 79, PB_PRED_SCALE = 80, PB_YN1 = 81, PB_YN2 = 82,
    PB_RATIO_HI = 83, PB_RATIO_LO = 84, PB_FRAC = 85, PB_LAST = 86,
    PB_LOOP_PS = 90, PB_LOOP_YN1 = 91, PB_LOOP_YN2 = 92,
    PB_LPF_ON = 93, PB_LPF_YN1 = 94, PB_LPF_A0 = 95, PB_LPF_B0 = 96,
    PB_LOOP_COUNTER = 97,
    PB_WORDS = 122,      // including padding (read), 98 words written back
    PB_WRITE_WORDS = 98,
};

constexpr int kSpms = 32;           // samples per millisecond (AX always mixes at 32 kHz)
constexpr int kFrame = 5 * kSpms;   // 160 samples per 5 ms frame

// Mix buses.
enum Bus { MAIN_L, MAIN_R, MAIN_S, AUXA_L, AUXA_R, AUXA_S, AUXB_L, AUXB_R, AUXB_S, NUM_BUSES };
int g_bus[NUM_BUSES][kFrame];

std::atomic<uint64_t> s_cmdlists{0}, s_voices{0};
std::atomic<uint32_t> s_active{0};
std::atomic<uint32_t> speech_pb{0};
struct WeaponCapture {std::atomic<uint64_t> sequence{0};std::atomic<uint32_t> record{0},handle{0};};
std::array<WeaponCapture,64> weapon_captures;
std::atomic<bool> weapon_capture_enabled{false};
std::atomic<uint64_t> speech_sequence{0},speech_output_frames{0},speech_target_frames{0},speech_skipped_frames{0};
bool g_ax_trace = getenv("SSNE_TRACE_AX") != nullptr;

// ------------------------------------------------------------------ sample accelerator
struct Accel {
    uint32_t start, end, cur;
    uint16_t format;
    int16_t yn1, yn2, gain;
    uint16_t ps;
    bool stopped = false;
    uint16_t* pb;   // PB words (for loop handling)

    int16_t read() {
        if (stopped) return 0;
        int size = format & 3, decode = (format >> 2) & 3, gscale = (format >> 4) & 3;
        int ci = (ps >> 4) & 7;
        int32_t c1 = int16_t(pb[PB_COEFS + ci * 2]), c2 = int16_t(pb[PB_COEFS + ci * 2 + 1]);

        uint16_t raw = 0;
        switch (size) {
        case 0: raw = aram8(cur >> 1); raw = (cur & 1) ? (raw & 0xF) : (raw >> 4); break;
        case 1: raw = aram8(cur); break;
        case 2: raw = uint16_t(aram8(cur * 2) << 8 | aram8(cur * 2 + 1)); break;
        default: break;
        }
        int16_t val = 0;
        int step = 2;
        if (decode == 0) {  // DSP-ADPCM
            int s = raw & 0xF;
            if (s >= 8) s -= 16;
            int32_t v = (int32_t(1 << (ps & 0xF)) * s) + ((0x400 + c1 * yn1 + c2 * yn2) >> 11);
            val = clamp16(v);
            yn2 = yn1; yn1 = val;
            cur += 1;
            if ((end & 0xF) == 0 && cur == end) cur = start + 1;
            else if ((end & 0xF) == 1 && cur == end - 1) cur = start;
            else if ((cur & 15) == 0) {
                ps = aram8((cur & ~15u) >> 1) & 0x7F;
                cur += 2;
                step += 2;
            }
        } else {  // PCM (decode 2) / MMIO modes (treated as PCM from ARAM)
            int shift = gscale == 1 ? 0 : gscale == 2 ? 16 : 11;
            int32_t v = ((int32_t(gain) * int16_t(raw)) >> shift) + ((c1 * yn1) >> shift) + ((c2 * yn2) >> shift);
            val = int16_t(v);
            yn2 = yn1; yn1 = val;
            if (decode != 1) cur += 1;
        }
        if (cur == end + step - 1) {  // end of sample reached
            cur = start;
            stopped = true;
            if (pb[PB_LOOPING]) {
                ps = pb[PB_LOOP_PS] & 0x7F;
                if (pb[PB_IS_STREAM] != 1) { yn1 = int16_t(pb[PB_LOOP_YN1]); yn2 = int16_t(pb[PB_LOOP_YN2]); }
                else pb[PB_LOOP_COUNTER]++;
                stopped = false;
            } else {
                pb[PB_RUNNING] = 0;
            }
        }
        cur &= 0xBFFFFFFF;
        return val;
    }
};

inline uint32_t hilo(const uint16_t* w, int i) { return uint32_t(w[i]) << 16 | w[i + 1]; }

// ------------------------------------------------------------------ resampling
constexpr double kPi = 3.14159265358979323846;
std::atomic<bool> g_hq{getenv("SSNE_AUDIO_ORIGINAL") == nullptr};

// 4-tap polyphase (src type 0). The DSP takes 128-phase x 4-tap coefficient tables from its ROM, selected by
// PB_COEF_SEL (the SDK's AX_SRC_TYPE_4TAP_8K / 12K / 16K). The ROM is not available, so equivalent windowed-sinc
// low-pass tables are generated: cutoffs of 1/2, 3/4 and all of the input band (the 16K table is exact at frac 0).
struct Poly4 { int16_t c[3][128][4]; };
const Poly4& poly4() {
    static const Poly4 tab = [] {
        Poly4 t{};
        const double cut[3] = {0.25, 0.375, 0.5};  // cycles per input sample
        for (int sel = 0; sel < 3; sel++)
            for (int ph = 0; ph < 128; ph++) {
                double f = ph / 128.0, h[4], sum = 0;
                for (int k = 0; k < 4; k++) {
                    double x = (k - 1) - f;  // taps at offsets -1, 0, +1, +2 around the interpolation point
                    double a = 2 * cut[sel] * x;
                    double sinc = std::abs(a) < 1e-9 ? 1.0 : std::sin(kPi * a) / (kPi * a);
                    double w = std::abs(x) >= 2 ? 0.0 : 0.5 * (1 + std::cos(kPi * x / 2));
                    h[k] = sinc * w;
                    sum += h[k];
                }
                for (int k = 0; k < 4; k++) t.c[sel][ph][k] = int16_t(std::lround(h[k] / sum * 32767.0));
            }
        return t;
    }();
    return tab;
}

// High quality: 64-tap Kaiser-windowed sinc (about 70 dB stopband). The cutoff follows the voice's rate ratio so that downsampling
// (ratio > 1) is band-limited to the 16 kHz output band. Tables are built per cutoff bucket on first use.
constexpr int kTaps = 64, kHistory = 128, kHalf = kTaps / 2, kPhases = 256;
double bessel_i0(double x) {
    double s = 1, t = 1;
    for (int k = 1; k < 30; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; }
    return s;
}
struct SincTable { float c[kPhases + 1][kTaps]; };
const SincTable& sinc_table(int bucket) {  // bucket: cutoff scale min(1, 1/ratio) in 1/64 steps (1..64)
    static std::unique_ptr<SincTable> tabs[65];
    auto& t = tabs[bucket];
    if (!t) {
        t = std::make_unique<SincTable>();
        // Cutoff in cycles per input sample: the transition band (~0.067) ends at the output band edge.
        const double fc = 0.5 * bucket / 64.0 - 0.034;
        const double beta = 7.0, i0b = bessel_i0(beta);
        for (int ph = 0; ph <= kPhases; ph++) {
            double f = double(ph) / kPhases, sum = 0, h[kTaps];
            for (int k = 0; k < kTaps; k++) {
                double x = (k - (kHalf - 1)) - f;  // taps at offsets -31 .. +32
                double a = 2 * fc * x;
                double sinc = std::abs(a) < 1e-9 ? 1.0 : std::sin(kPi * a) / (kPi * a);
                double r = x / kHalf;
                double w = std::abs(r) >= 1 ? 0.0 : bessel_i0(beta * std::sqrt(1 - r * r)) / i0b;
                h[k] = sinc * w;
                sum += h[k];
            }
            for (int k = 0; k < kTaps; k++) t->c[ph][k] = float(h[k] / sum);
        }
    }
    return *t;
}

// Input history for the high-quality filter, kept on the host per parameter block. PB_LAST still holds the four
// newest samples; when they don't match (voice restarted or switched modes) the history is reset from them.
struct VoiceHist { int16_t h[kHistory]; uint32_t n; };
std::unordered_map<uint32_t, VoiceHist> g_hist;

void get_input_samples(uint16_t* pb, uint32_t pb_addr, int16_t* out, int count) {
    Accel a;
    a.pb = pb;
    a.start = hilo(pb, PB_LOOP_HI) & 0x3FFFFFFF;
    a.end = hilo(pb, PB_END_HI) & 0x3FFFFFFF;
    a.cur = hilo(pb, PB_CUR_HI) & 0xBFFFFFFF;
    a.format = pb[PB_FORMAT];
    a.yn1 = int16_t(pb[PB_YN1]);
    a.yn2 = int16_t(pb[PB_YN2]);
    a.gain = int16_t(pb[PB_GAIN]);
    a.ps = pb[PB_PRED_SCALE] & 0x7F;

    uint32_t ratio = hilo(pb, PB_RATIO_HI);
    uint32_t pos = pb[PB_FRAC];
    int16_t* last = reinterpret_cast<int16_t*>(pb + PB_LAST);
    int src_type = pb[PB_SRC_TYPE];

    if ((src_type == 0 || src_type == 1) && g_hq.load(std::memory_order_relaxed)) {
        VoiceHist& vh = g_hist[pb_addr];
        bool match = vh.n >= 4;
        for (int k = 0; k < 4 && match; k++) match = vh.h[(vh.n - 4 + k) % kHistory] == last[k];
        if (!match) {
            memset(vh.h, 0, sizeof(vh.h));
            vh.n = kHistory;
            for (int k = 0; k < 4; k++) vh.h[kHistory - 4 + k] = last[k];
        }
        double scale = ratio > 0x10000 ? double(0x10000) / ratio : 1.0;
        int bucket = std::clamp(int(std::ceil(scale * 64 - 1e-6)), 1, 64);
        const SincTable& tab = sinc_table(bucket);
        uint32_t n = vh.n;
        for (int i = 0; i < count; i++) {
            pos += ratio;
            while (pos >= 0x10000) { vh.h[n++ % kHistory] = a.read(); pos -= 0x10000; }
            uint16_t frac = uint16_t(pos);
            // Interpolate between samples n-1-kHalf and n-kHalf (a fixed kHalf-sample delay gives the lookahead).
            uint32_t base = n - kTaps;  // oldest tap
            if (!frac) { out[i] = vh.h[(base + kHalf - 1) % kHistory]; continue; }
            uint32_t pf = uint32_t(frac) * kPhases;
            int ph = int(pf >> 16);
            float w = float(pf & 0xFFFF) * (1.0f / 65536.0f);
            const float* c0 = tab.c[ph];
            const float* c1 = tab.c[ph + 1];
            float acc = 0;
            for (int k = 0; k < kTaps; k++) {
                float c = c0[k] + (c1[k] - c0[k]) * w;
                acc += c * vh.h[(base + k) % kHistory];
            }
            out[i] = clamp16(std::lround(acc));
        }
        vh.n = n;
        for (int k = 0; k < 4; k++) last[k] = vh.h[(n - 4 + k) % kHistory];
    } else if (src_type == 0) {
        // DSP 4-tap polyphase: interpolate between t[n-3] and t[n-2] using t[n-4 .. n-1].
        const auto& c = poly4().c[std::min<int>(pb[PB_COEF_SEL], 2)];
        int16_t t[4] = {last[0], last[1], last[2], last[3]};
        uint32_t idx = 4;
        for (int i = 0; i < count; i++) {
            pos += ratio;
            while (pos >= 0x10000) { t[idx++ & 3] = a.read(); pos -= 0x10000; }
            const int16_t* cc = c[uint16_t(pos) >> 9];
            int32_t s = 0;
            for (int k = 0; k < 4; k++) s += int32_t(t[(idx + k) & 3]) * cc[k];
            out[i] = clamp16((s + 0x4000) >> 15);
        }
        for (int k = 0; k < 4; k++) last[k] = t[(idx + k) & 3];
    } else if (src_type == 1) {
        int16_t t[4] = {last[0], last[1], last[2], last[3]};
        uint32_t idx = 4;
        for (int i = 0; i < count; i++) {
            pos += ratio;
            while (pos >= 0x10000) { t[idx++ & 3] = a.read(); pos -= 0x10000; }
            uint16_t frac = uint16_t(pos);
            int16_t s;
            if (frac) {
                // Interpolate between the two most recent input samples.
                int32_t s0 = t[(idx - 2) & 3], s1 = t[(idx - 1) & 3];
                s = int16_t((s0 * int32_t(uint16_t(-frac)) + s1 * int32_t(frac)) >> 16);
            } else {
                s = t[(idx - 2) & 3];
            }
            out[i] = s;
        }
        for (int k = 0; k < 4; k++) last[k] = t[(idx + k) & 3];
    } else {  // nearest / no conversion
        for (int i = 0; i < count; i++) out[i] = a.read();
        memcpy(last, out + count - 4, 8);
    }
    pb[PB_FRAC] = uint16_t(pos);
    pb[PB_CUR_HI] = uint16_t(a.cur >> 16);
    pb[PB_CUR_LO] = uint16_t(a.cur);
    pb[PB_YN1] = uint16_t(a.yn1);
    pb[PB_YN2] = uint16_t(a.yn2);
    pb[PB_PRED_SCALE] = a.ps;
}

void mix_add(int* out, const int16_t* in, int count, uint16_t* vol, int16_t* dpop, bool ramp) {
    uint16_t v = vol[0];
    uint16_t d = ramp ? vol[1] : 0;
    for (int i = 0; i < count; i++) {
        int16_t s = clamp16((int64_t(in[i]) * v) >> 15);
        out[i] += s;
        v = uint16_t(v + d);
        *dpop = s;
    }
    vol[0] = v;
}

// Separate volumes: music and video sound are the streamed voices (DSP-ADPCM ring buffers refilled from
// StreamData.dat) and the PCM16 voices (Bink video audio, streamed music); everything else (DSP-ADPCM one-shots
// from the sound bank) is sound effects. Applied per voice before the game's own mixer volumes.
std::atomic<int> g_music_pct{100}, g_sfx_pct{100};
bool is_music_voice(const uint16_t* pb) { return pb[PB_IS_STREAM] == 1 || pb[PB_FORMAT] == 0x0A; }

// Advance near-silence through the existing decoder; never assign a compressed cursor or
// bypass ADPCM/SRC history. Stay within the current ARAM refill half and defer
// while native DVD/ARAM transfers are active. At most5ms per DSP millisecond.
void reconcile_speech_decoder(uint16_t* pb,uint32_t address) {
    if(speech_pb.load(std::memory_order_acquire)!=(address&(kRamSize-1)))return;
    const auto target=speech_target_frames.load(std::memory_order_acquire),current=speech_output_frames.load(std::memory_order_relaxed);
    if(target<=current+320)return; //10ms deadband
    const auto channel=mem_read32(0x802DCCC0);if(channel>6)return;
    if(mem_read32(0x802D59D0+channel*188+8))return;
    for(unsigned i=0;i<16;++i)if(!mem_read32(0x802D668C+i*36+0x20))return;
    const auto bytes=mem_read32(0x802DCCC8); // native StreamData half-buffer bytes (SDA -55D8)
    const auto size=pb[PB_FORMAT]&3;
    if(!bytes || bytes>0x100000 || size>2)return;
    const uint64_t half_units=size==0?uint64_t(bytes)*2:size==2?bytes/2:bytes;
    const auto start=hilo(pb,PB_LOOP_HI),end=hilo(pb,PB_END_HI);
    unsigned left=unsigned(std::min<uint64_t>(target-current-320,160));
    while(left>=4 && pb[PB_RUNNING]==1) {
        const auto cursor=hilo(pb,PB_CUR_HI);
        if(cursor<start || cursor>=end || !half_units)break;
        const auto boundary=std::min<uint64_t>(end,uint64_t(start)+((cursor-start)/half_units+1)*half_units);
        const unsigned block=std::min<unsigned>(left,kSpms);
        const auto ratio=hilo(pb,PB_RATIO_HI);
        if(ratio>0x40000)break;
        const uint64_t input=(uint64_t(block)*std::max<uint32_t>(ratio,0x10000)+65535)/65536+128;
        const uint64_t needed=size==0?(input*16+13)/14:input;
        if(boundary<=cursor || boundary-cursor<=needed)break;
        // Preview with complete decoder/SRC state rollback. Drop only genuine
        // near-silence; do not cut voiced samples to force a smaller offset.
        uint16_t previous[PB_WORDS];memcpy(previous,pb,sizeof(previous));
        auto found=g_hist.find(address);const bool had_history=found!=g_hist.end();
        VoiceHist old_history{};if(had_history)old_history=found->second;
        int16_t discarded[kSpms];get_input_samples(pb,address,discarded,block);
        int peak=0;for(unsigned i=0;i<block;++i)peak=std::max(peak,std::abs(int(discarded[i])));
        if(peak>128){memcpy(pb,previous,sizeof(previous));if(had_history)g_hist[address]=old_history;else g_hist.erase(address);break;}
        // Advance envelope/filter histories for decoded, inaudible catchup.
        for(unsigned i=0;i<block;++i){discarded[i]=clamp16((int32_t(discarded[i])*int16_t(pb[PB_VE_CUR]))>>15);pb[PB_VE_CUR]=uint16_t(pb[PB_VE_CUR]+pb[PB_VE_DELTA]);}
        if(pb[PB_LPF_ON]){int16_t value=int16_t(pb[PB_LPF_YN1]);for(unsigned i=0;i<block;++i)value=clamp16((int32_t(pb[PB_LPF_A0])*discarded[i]+int16_t(pb[PB_LPF_B0])*int32_t(value))>>16);pb[PB_LPF_YN1]=uint16_t(value);}
        speech_output_frames.fetch_add(block,std::memory_order_relaxed);speech_skipped_frames.fetch_add(block,std::memory_order_relaxed);left-=block;
    }
}

void process_voice(uint16_t* pb, uint32_t pb_addr, int* const bus[NUM_BUSES], int count) {
    if (pb[PB_RUNNING] != 1) return;
    if(music_capture_probe() && speech_sequence.load() && is_music_voice(pb)){static std::unordered_map<uint32_t,bool> seen;if(seen.emplace(pb_addr,true).second)log_message("[coop-speech-dsp-block] actual="+std::to_string(pb_addr)+" tracked="+std::to_string(speech_pb.load()));}
    reconcile_speech_decoder(pb,pb_addr);
    int16_t smp[kSpms];
    get_input_samples(pb, pb_addr, smp, count);
    if(speech_pb.load(std::memory_order_acquire)==(pb_addr&(kRamSize-1)))speech_output_frames.fetch_add(count,std::memory_order_relaxed);
    if (int pct = is_music_voice(pb) ? g_music_pct.load() : g_sfx_pct.load(); pct != 100)
        for (int i = 0; i < count; i++) smp[i] = clamp16(int32_t(smp[i]) * pct / 100);

    for (int i = 0; i < count; i++) {
        int32_t vol = int16_t(pb[PB_VE_CUR]);
        smp[i] = clamp16((int32_t(smp[i]) * vol) >> 15);
        pb[PB_VE_CUR] = uint16_t(pb[PB_VE_CUR] + pb[PB_VE_DELTA]);
    }
    if (pb[PB_LPF_ON]) {
        int32_t a0 = pb[PB_LPF_A0], b0 = int16_t(pb[PB_LPF_B0]);
        int16_t y = int16_t(pb[PB_LPF_YN1]);
        for (int i = 0; i < count; i++) y = smp[i] = clamp16((a0 * smp[i] + b0 * y) >> 15);
        pb[PB_LPF_YN1] = uint16_t(y);
    }

    uint16_t mc = pb[PB_MIXER_CTRL];
    // Diagnostic voice-only capture remains valid while background music plays.
    // Capture actual post-envelope/filter samples, before unrelated bus mixing.
    if(music_capture_probe() &&
       speech_pb.load()==(pb_addr&(kRamSize-1)) && native_speech_playing_track()!=UINT32_MAX){
        struct Capture{uint64_t sequence=0,frames=0;FILE* file=nullptr;~Capture(){if(file)std::fclose(file);}};
        static Capture voice;const auto event=speech_sequence.load();
        if(event && voice.sequence!=event){if(voice.file)std::fclose(voice.file);voice.sequence=event;voice.frames=0;const auto name="audio-speech-decoder-"+std::to_string(event)+".pcm";voice.file=std::fopen(name.c_str(),"wb");}
        if(voice.file && voice.frames+count<=32000*60){std::fwrite(smp,sizeof(int16_t),count,voice.file);voice.frames+=count;std::fflush(voice.file);}
    }
    if(weapon_capture_enabled.load(std::memory_order_relaxed) && !is_music_voice(pb)) {
        const auto physical=pb_addr&(kRamSize-1);constexpr uint32_t pool=0x002C18A0u;
        if(physical>=pool && physical<pool+64*236 && (physical-pool)%236==0) {
            const auto slot=(physical-pool)/236;auto& trace=weapon_captures[slot];const auto event=trace.sequence.load(std::memory_order_acquire);const auto record=trace.record.load();const auto handle=trace.handle.load();
            if(event && record>=0x80000000u && record<=guest_ram_end()-0xC0 && rd32(record)!=0 && rd32(record+0xAC)==handle) {
                struct Output{uint64_t sequence=0,frames=0,nonzero=0,stereo_nonzero=0;FILE* file=nullptr;FILE* stereo=nullptr;};static std::array<Output,64> outputs;auto& output=outputs[slot];
                if(output.sequence!=event){if(output.file)std::fclose(output.file);if(output.stereo)std::fclose(output.stereo);output={event,0,0,0,nullptr,nullptr};const auto name="audio-weapon-shot-"+std::to_string(event);output.file=std::fopen((name+".pcm").c_str(),"wb");output.stereo=std::fopen((name+".stereo.pcm").c_str(),"wb");}
                if(output.file && output.frames+count<=64000){
                    std::fwrite(smp,sizeof(int16_t),count,output.file);output.frames+=count;for(int i=0;i<count;++i)output.nonzero+=smp[i]!=0;
                    // Actual contribution to mainL/mainR, using the same gains
                    // and ramp as mix_add below, without modifying the mixer.
                    int16_t stereo[kSpms*2];uint16_t left=pb[PB_MIXER],right=pb[PB_MIXER+2];const auto dl=mc&8?pb[PB_MIXER+1]:0,dr=mc&8?pb[PB_MIXER+3]:0;
                    for(int i=0;i<count;++i){stereo[i*2]=mc&1?clamp16((int64_t(smp[i])*left)>>15):0;stereo[i*2+1]=mc&2?clamp16((int64_t(smp[i])*right)>>15):0;output.stereo_nonzero+=(stereo[i*2]!=0)+(stereo[i*2+1]!=0);left=uint16_t(left+dl);right=uint16_t(right+dr);}
                    if(output.stereo)std::fwrite(stereo,sizeof(int16_t)*2,count,output.stereo);
                    if(output.frames==unsigned(count) || output.frames%3200==0){std::fflush(output.file);if(output.stereo)std::fflush(output.stereo);log_message("[coop-weapon-shot-pcm] sequence="+std::to_string(event)+" dsp_slot="+std::to_string(slot)+" frames="+std::to_string(output.frames)+" nonzero="+std::to_string(output.nonzero)+" stereo_nonzero="+std::to_string(output.stereo_nonzero)+" rate=32000");
                        if(event>=0x300000000ull && event<0x300000010ull)log_message("[dev-spatial-mix] sequence="+std::to_string(event)+" frames="+std::to_string(output.frames)+" distance="+std::to_string(u2f(rd32(record+0x2C)))+" pan="+std::to_string(u2f(rd32(record+0x24)))+" spatial_db="+std::to_string(int32_t(rd32(record+0x44)))+" volume_db="+std::to_string(int32_t(rd32(record+0x50)))+" mix_input="+std::to_string(int32_t(rd32(0x802D2CA0u+slot*96+8)))+" mix_control="+std::to_string(mc)+" left="+std::to_string(pb[PB_MIXER])+" delta="+std::to_string(int16_t(pb[PB_MIXER+1]))+" right="+std::to_string(pb[PB_MIXER+2]));
                    }
                }
            }
        }
    }
    auto dpop = [&](int k) { return reinterpret_cast<int16_t*>(pb + PB_DPOP + k); };
    auto vol = [&](int k) { return pb + PB_MIXER + 2 * k; };
    // mixer slots: 0 mainL 1 mainR 2 auxAL 3 auxAR 4 auxBL 5 auxBR 6 auxBS 7 mainS 8 auxAS
    // dpop slots:  0 mainL 1 auxAL 2 auxBL 3 mainR 4 auxAR 5 auxBR 6 mainS 7 auxAS 8 auxBS
    if (mc & 0x0001) mix_add(bus[MAIN_L], smp, count, vol(0), dpop(0), mc & 0x0008);
    if (mc & 0x0002) mix_add(bus[MAIN_R], smp, count, vol(1), dpop(3), mc & 0x0008);
    if (mc & 0x0004) mix_add(bus[MAIN_S], smp, count, vol(7), dpop(6), mc & 0x0008);
    if (mc & 0x0010) mix_add(bus[AUXA_L], smp, count, vol(2), dpop(1), mc & 0x0040);
    if (mc & 0x0020) mix_add(bus[AUXA_R], smp, count, vol(3), dpop(4), mc & 0x0040);
    if (mc & 0x0080) mix_add(bus[AUXA_S], smp, count, vol(8), dpop(7), mc & 0x0100);
    if (mc & 0x0200) mix_add(bus[AUXB_L], smp, count, vol(4), dpop(2), mc & 0x0800);
    if (mc & 0x0400) mix_add(bus[AUXB_R], smp, count, vol(5), dpop(5), mc & 0x0800);
    if (mc & 0x1000) mix_add(bus[AUXB_S], smp, count, vol(6), dpop(8), mc & 0x2000);
}

void process_pb_list(uint32_t pb_addr) {
    uint16_t pb[PB_WORDS];
    uint32_t active = 0;
    int guard = 0;
    while (pb_addr && guard++ < 256) {
        pb_addr &= kRamSize - 1;
        for (int i = 0; i < PB_WORDS; i++) pb[i] = rd16(pb_addr + i * 2);

        uint32_t upd_addr = hilo(pb, PB_UPD_HI);
        uint16_t nupd[5];
        memcpy(nupd, pb + PB_NUM_UPDATES, sizeof(nupd));
        uint32_t upd_idx = 0;
        if (pb[PB_RUNNING] == 1) {
            active++;
            if (g_ax_trace && s_cmdlists % 200 == 0)
                fprintf(stdout, "[ax] pb %08X fmt %04X src %u coef %u ratio %08X mc %04X ve %04X/%04X lpf %u loop %u stream %u "
                        "cur %08X end %08X loopa %08X gain %04X mixL %04X mixR %04X\n", pb_addr, pb[PB_FORMAT],
                        pb[PB_SRC_TYPE], pb[PB_COEF_SEL], hilo(pb, PB_RATIO_HI), pb[PB_MIXER_CTRL], pb[PB_VE_CUR], pb[PB_VE_DELTA],
                        pb[PB_LPF_ON], pb[PB_LOOPING], pb[PB_IS_STREAM], hilo(pb, PB_CUR_HI), hilo(pb, PB_END_HI),
                        hilo(pb, PB_LOOP_HI), pb[PB_GAIN], pb[PB_MIXER], pb[PB_MIXER + 2]);
        }

        int* bus[NUM_BUSES];
        for (int b = 0; b < NUM_BUSES; b++) bus[b] = g_bus[b];
        for (int ms = 0; ms < 5; ms++) {
            for (uint32_t u = 0; u < nupd[ms] && upd_idx < 1024; u++, upd_idx++) {
                uint16_t off = rd16(upd_addr + upd_idx * 4), val = rd16(upd_addr + upd_idx * 4 + 2);
                if (off < PB_WORDS) pb[off] = val;
            }
            process_voice(pb, pb_addr, bus, kSpms);
            for (auto& p : bus) p += kSpms;
        }
        for (int i = 0; i < PB_WRITE_WORDS; i++) wr16(pb_addr + i * 2, pb[i]);
        s_voices++;
        pb_addr = hilo(pb, PB_NEXT_HI);
    }
    s_active = active;
}

void upload(uint32_t addr, const int* buf) {
    for (int i = 0; i < kFrame; i++) wr32(addr + i * 4, uint32_t(buf[i]));
}

void mix_aux(int aux, uint32_t write_addr, uint32_t read_addr) {
    int base = aux == 0 ? AUXA_L : AUXB_L;
    if (write_addr)
        for (int k = 0; k < 3; k++) upload(write_addr + k * kFrame * 4, g_bus[base + k]);
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < kFrame; i++) g_bus[MAIN_L + k][i] += int32_t(rd32(read_addr + (k * kFrame + i) * 4));
}

} // namespace

void ax_process_cmdlist(uint32_t addr, uint16_t size_words) {
    (void)size_words;
    s_cmdlists++;
    uint32_t p = addr & (kRamSize - 1);
    uint32_t pb_addr = 0;
    auto next = [&]() { uint16_t v = rd16(p); p += 2; return v; };
    auto next32 = [&]() { uint32_t hi = next(); return hi << 16 | next(); };
    for (int n = 0; n < 4096; n++) {
        uint16_t cmd = next();
        if (g_ax_trace && s_cmdlists < 4) fprintf(stdout, "[ax] cmd %02X\n", cmd);
        switch (cmd) {
        case 0x00: {  // SETUP: initial values (hi, lo, delta) for the 9 buses
            uint32_t a = next32();
            for (int b = 0; b < NUM_BUSES; b++) {
                int32_t v = int32_t(uint32_t(rd16(a + b * 6)) << 16 | rd16(a + b * 6 + 2));
                int16_t d = int16_t(rd16(a + b * 6 + 4));
                for (int i = 0; i < kFrame; i++) g_bus[b][i] = v ? v + i * d : 0;
            }
            break;
        }
        case 0x01: {  // DL_AND_VOL_MIX
            uint32_t a = next32();
            uint16_t vols[3] = {next(), next(), next()};
            for (int g = 0; g < 3; g++)
                for (int k = 0; k < 3; k++)
                    for (int i = 0; i < kFrame; i++) {
                        int64_t s = int32_t(rd32(a + (k * kFrame + i) * 4));
                        g_bus[g * 3 + k][i] += int32_t((s * vols[g]) >> 15);
                    }
            break;
        }
        case 0x02: pb_addr = next32(); break;
        case 0x03: process_pb_list(pb_addr); break;
        case 0x04:
        case 0x05: { uint32_t w = next32(), r = next32(); mix_aux(cmd - 4, w, r); break; }
        case 0x06: {  // UPLOAD_LRS
            uint32_t a = next32();
            for (int k = 0; k < 3; k++) upload(a + k * kFrame * 4, g_bus[MAIN_L + k]);
            break;
        }
        case 0x07: {  // SET_LR
            uint32_t a = next32();
            for (int i = 0; i < kFrame; i++) {
                int32_t s = int32_t(rd32(a + i * 4));
                g_bus[MAIN_L][i] = s; g_bus[MAIN_R][i] = s; g_bus[MAIN_S][i] = 0;
            }
            break;
        }
        case 0x08: p += 20; break;
        case 0x09: { uint32_t r = next32(); mix_aux(1, 0, r); break; }
        case 0x0A: case 0x0B: case 0x0C: break;
        case 0x0D: {  // MORE: continue with another command list
            uint32_t a = next32();
            next();
            p = a & (kRamSize - 1);
            break;
        }
        case 0x0E: {  // OUTPUT: surround upload + interleaved R,L s16
            uint32_t surround = next32(), lr = next32();
            upload(surround, g_bus[MAIN_S]);
            for (int i = 0; i < kFrame; i++) {
                wr16(lr + i * 4, uint16_t(clamp16(g_bus[MAIN_R][i])));
                wr16(lr + i * 4 + 2, uint16_t(clamp16(g_bus[MAIN_L][i])));
            }
            break;
        }
        case 0x0F: return;  // END
        case 0x10: {  // MIX_AUXB_LR
            uint32_t ul = next32(), dl = next32();
            upload(ul, g_bus[AUXB_L]);
            upload(ul + kFrame * 4, g_bus[AUXB_R]);
            for (int k = 0; k < 2; k++)
                for (int i = 0; i < kFrame; i++) {
                    int32_t s = int32_t(rd32(dl + (k * kFrame + i) * 4));
                    g_bus[AUXB_L + k][i] = s;
                    g_bus[MAIN_L + k][i] += s;
                }
            break;
        }
        case 0x11: {  // SET_OPPOSITE_LR
            uint32_t a = next32();
            for (int i = 0; i < kFrame; i++) {
                int32_t s = int32_t(rd32(a + i * 4));
                g_bus[MAIN_L][i] = -s; g_bus[MAIN_R][i] = s; g_bus[MAIN_S][i] = 0;
            }
            break;
        }
        case 0x12: {  // COMPRESSOR
            static uint32_t pos = 0;
            uint16_t threshold = next(), frames = next();
            uint32_t table = next32();
            bool trig = false;
            for (int i = 0; i < kFrame && !trig; i++)
                trig = std::abs(g_bus[MAIN_L][i]) > int(threshold) || std::abs(g_bus[MAIN_R][i]) > int(threshold);
            uint32_t off;
            if (trig) { off = pos * kFrame * 2; pos = frames; }
            else if (pos) { --pos; off = (11 + pos) * kFrame * 2; }
            else break;
            for (int i = 0; i < kFrame; i++) {
                uint16_t c = rd16(table + off + i * 2);
                g_bus[MAIN_L][i] = int32_t((int64_t(g_bus[MAIN_L][i]) * c) >> 15);
                g_bus[MAIN_R][i] = int32_t((int64_t(g_bus[MAIN_R][i]) * c) >> 15);
            }
            break;
        }
        case 0x13: {  // SEND_AUX_AND_MIX
            uint32_t auxa_up = next32(), auxb_s_up = next32();
            uint32_t dl[4] = {next32(), next32(), next32(), next32()};
            for (int k = 0; k < 3; k++) upload(auxa_up + k * kFrame * 4, g_bus[AUXA_L + k]);
            upload(auxb_s_up, g_bus[AUXB_S]);
            const int dst[4] = {MAIN_L, MAIN_R, AUXB_L, AUXB_R};
            for (int k = 0; k < 4; k++)
                for (int i = 0; i < kFrame; i++) g_bus[dst[k]][i] += int32_t(rd32(dl[k] + i * 4));
            break;
        }
        default:
            if (g_ax_trace) fprintf(stdout, "[ax] unknown command %04X\n", cmd);
            return;
        }
    }
}

// ------------------------------------------------------------------ host FIFO
namespace {
std::mutex g_fifo_mutex;
std::vector<int16_t> g_fifo(48000 * 2);   // 1 s of stereo at 48 kHz (ring)
size_t g_rd = 0, g_wr = 0, g_count = 0;  // in frames
std::atomic<uint32_t> g_rate{32000};
std::atomic<bool> g_sink{false};
std::atomic<uint64_t> s_ai_frames{0}, s_underruns{0}, s_overruns{0};
} // namespace

void audio_set_sink_active(bool active) { g_sink = active; }

void audio_push_ai(const uint8_t* src, uint32_t frames, uint32_t rate) {
    g_rate = rate;
    s_ai_frames += frames;
    if(music_capture_probe()) {
        static std::unordered_map<uint32_t,FILE*> voice_captures;
        static uint64_t voice_bytes=0;
        const auto voice_track=native_speech_playing_track();
        if(voice_track!=UINT32_MAX && !native_background_music_running() && voice_bytes+uint64_t(frames)*4<=64*1024*1024) {
            auto [voice,created]=voice_captures.try_emplace(voice_track,nullptr);
            if(created){const auto name="audio-speech-"+std::to_string(voice_track)+".pcm";voice->second=fopen(name.c_str(),"wb");}
            if(voice->second){fwrite(src,4,frames,voice->second);voice_bytes+=uint64_t(frames)*4;}
        }
        static FILE* pcm=fopen("audio-music.pcm","wb");
        static uint64_t captured=0,nonzero=0,last_report=0;
        if(pcm && captured*4+uint64_t(frames)*4<=64*1024*1024) {
            fwrite(src,4,frames,pcm);captured+=frames;
            for(unsigned i=0;i<frames*4;i+=2)nonzero+=(src[i]!=0 || src[i+1]!=0);
            if(captured-last_report>=rate*2) {
                last_report=captured;fflush(pcm);
                log_message("[coop-music-pcm] rate="+std::to_string(rate)+" frames="+std::to_string(captured)+" nonzero_samples="+std::to_string(nonzero)+" current_track="+std::to_string(mem_read32(0x802DCD10))+" speech_track="+std::to_string(native_speech_playing_track()));
            }
            if(captured==frames)if(auto* info=fopen("audio-music.rate","w")){fprintf(info,"%u\n",rate);fclose(info);}
        }
    }
    if (!g_sink) return;
    std::lock_guard<std::mutex> lk(g_fifo_mutex);
    size_t cap = g_fifo.size() / 2;
    for (uint32_t i = 0; i < frames; i++) {
        int16_t r = int16_t(src[i * 4] << 8 | src[i * 4 + 1]);
        int16_t l = int16_t(src[i * 4 + 2] << 8 | src[i * 4 + 3]);
        if (g_count == cap) { g_rd = (g_rd + 1) % cap; g_count--; s_overruns++; }
        g_fifo[g_wr * 2] = l;
        g_fifo[g_wr * 2 + 1] = r;
        g_wr = (g_wr + 1) % cap;
        g_count++;
    }
}

uint32_t audio_pull(int16_t* out, uint32_t frames) {
    std::lock_guard<std::mutex> lk(g_fifo_mutex);
    size_t cap = g_fifo.size() / 2;
    uint32_t n = uint32_t(std::min<size_t>(frames, g_count));
    for (uint32_t i = 0; i < n; i++) {
        out[i * 2] = g_fifo[g_rd * 2];
        out[i * 2 + 1] = g_fifo[g_rd * 2 + 1];
        g_rd = (g_rd + 1) % cap;
    }
    g_count -= n;
    if (n < frames) {
        s_underruns++;
        memset(out + n * 2, 0, (frames - n) * 4);
    }
    return n;
}

void audio_set_hq(bool hq) { g_hq = hq; }
void audio_set_mix(int music_pct, int sfx_pct) { if(music_capture_probe()){music_pct=100;sfx_pct=0;} g_music_pct = std::clamp(music_pct, 0, 100); g_sfx_pct = std::clamp(sfx_pct, 0, 100); }
bool audio_hq() { return g_hq; }

void audio_track_weapon_voice(uint32_t parameter_block,uint32_t sound_record,uint32_t handle,uint64_t sequence) {
    if(!getenv("SSNE_TEST_HIDDEN") || !getenv("SSNE_COOP_TRACE_LIFETIMES") || !sequence || parameter_block<0x802C18A0u || parameter_block>=0x802C18A0u+64*236 || (parameter_block-0x802C18A0u)%236)return;
    auto& trace=weapon_captures[(parameter_block-0x802C18A0u)/236];trace.sequence.store(0,std::memory_order_release);trace.record=sound_record;trace.handle=handle;trace.sequence.store(sequence,std::memory_order_release);weapon_capture_enabled=true;
}
void audio_track_speech_voice(uint32_t parameter_block,uint64_t sequence) {
    speech_pb.store(0,std::memory_order_release);
    speech_output_frames.store(0,std::memory_order_relaxed);
    speech_target_frames.store(0,std::memory_order_relaxed);speech_skipped_frames.store(0,std::memory_order_relaxed);
    speech_sequence.store(sequence,std::memory_order_release);
    speech_pb.store(parameter_block&(kRamSize-1),std::memory_order_release);
    log_message("[coop-speech-dsp-binding] sequence="+std::to_string(sequence)+" pb="+std::to_string(parameter_block&(kRamSize-1)));
}
uint64_t audio_speech_output_frames(uint64_t sequence) {
    return sequence && speech_sequence.load(std::memory_order_acquire)==sequence?speech_output_frames.load(std::memory_order_relaxed):0;
}
void audio_reconcile_speech(uint64_t sequence,uint64_t target_frames) {
    if(!sequence || speech_sequence.load(std::memory_order_acquire)!=sequence || target_frames>32000ull*3600)return;
    const auto current=speech_output_frames.load(std::memory_order_relaxed);
    speech_target_frames.store(std::min(target_frames,current+3200),std::memory_order_release); //100ms per received update
}
uint64_t audio_speech_skipped_frames(uint64_t sequence) {
    return sequence && speech_sequence.load(std::memory_order_acquire)==sequence?speech_skipped_frames.load(std::memory_order_relaxed):0;
}
uint32_t audio_rate() { return g_rate; }
AudioSpeechCheckpoint audio_speech_checkpoint(){return {speech_pb.load(),speech_sequence.load(),speech_output_frames.load(),speech_target_frames.load(),speech_skipped_frames.load()};}
void audio_restore_speech_checkpoint(const AudioSpeechCheckpoint& state){
    speech_pb=0;speech_sequence=state.sequence;speech_output_frames=state.output;
    speech_target_frames=state.target;speech_skipped_frames=state.skipped;speech_pb=state.parameter_block;
}
uint32_t audio_buffered() { std::lock_guard<std::mutex> lk(g_fifo_mutex); return uint32_t(g_count); }

AudioStats audio_stats() {
    return {s_cmdlists.load(), s_voices.load(), s_ai_frames.load(), s_underruns.load(), s_overruns.load(), s_active.load()};
}

void audio_state_reset() {
    g_hist.clear();  // guest thread (AX runs there too)
    std::lock_guard<std::mutex> lk(g_fifo_mutex);
    g_count = 0;
    g_rd = g_wr;
}

} // namespace ssne
