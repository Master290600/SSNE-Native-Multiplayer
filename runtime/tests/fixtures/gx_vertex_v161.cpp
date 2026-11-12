#include "ssne/performance_detail.h"
#include "ssne/runtime.h"
// Vertex loader + transform unit (XF): attribute decode, matrices, lighting, texgen.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>

#include "gx_internal.h"
#include "gx_vertex_bytes.h"
#include "gx_vertex_plan.h"
#include <type_traits>
#include "ssne/ppc_context.h"

namespace ssne::gx {

namespace {

int comp_size(uint32_t fmt) { static const int s[8] = {1, 1, 2, 2, 4, 0, 0, 0}; return s[fmt & 7]; }
int color_size(uint32_t fmt) { static const int s[8] = {2, 3, 4, 2, 3, 4, 0, 0}; return s[fmt & 7]; }
int index_size(uint32_t type) { return type == 2 ? 1 : type == 3 ? 2 : 0; }

// VAT field locations for texcoords 0..7: (register 0=A 1=B 2=C, cnt bit, fmt bit, shift bit)
struct TexVat { int reg, cnt, fmt, shift; };
const TexVat kTexVat[8] = {{0, 21, 22, 25}, {1, 0, 1, 4}, {1, 9, 10, 13}, {1, 18, 19, 22},
                           {1, 27, 28, -1}, {2, 5, 6, 9}, {2, 14, 15, 18}, {2, 23, 24, 27}};

uint32_t vat_reg(int vat, int r) { return g.cp[(r == 0 ? 0x70 : r == 1 ? 0x80 : 0x90) + vat]; }
uint32_t tex_shift(int vat, int t) {
    if (t == 4) return vat_reg(vat, 2) & 31;  // TEX4 shift lives in VAT C bits 0-4
    return (vat_reg(vat, kTexVat[t].reg) >> kTexVat[t].shift) & 31;
}

// Reads one component from guest memory or the command stream.
struct Reader {
    const uint8_t* p = nullptr;  // command stream (direct data)
    uint32_t addr = 0;           // guest memory (indexed data)
    bool mem = false;
    uint8_t u8(uint32_t o) const { return mem ? mem_read8(addr + o) : p[o]; }
    uint16_t u16(uint32_t o) const {
        if (!mem) { uint16_t v; memcpy(&v, p + o, 2); return SSNE_BSWAP16(v); }
        return uint16_t(u8(o) << 8 | u8(o + 1));
    }
    uint32_t u32(uint32_t o) const {
        if (!mem) { uint32_t v; memcpy(&v, p + o, 4); return SSNE_BSWAP32(v); }
        return uint32_t(u16(o)) << 16 | u16(o + 2);
    }
    float comp(uint32_t fmt, uint32_t i, float scale) const {
        switch (fmt) {
        case 0: return u8(i) * scale;
        case 1: return int8_t(u8(i)) * scale;
        case 2: return u16(i * 2) * scale;
        case 3: return int16_t(u16(i * 2)) * scale;
        default: { uint32_t v = u32(i * 4); float f; memcpy(&f, &v, 4); return f; }
        }
    }
    // Dispatch the source format once per attribute. The usual float RAM /
    // FIFO input can be endian-converted directly, avoiding a complete Reader
    // component switch and function call for every scalar. The checked memory
    // path retains the original reader, including its MMIO ordering.
    void components(uint32_t fmt,uint32_t count,float scale,float* out) const {
        if(!mem && fmt>=4){
            for(uint32_t k=0;k<count;++k){uint32_t bits;std::memcpy(&bits,p+k*4,4);bits=SSNE_BSWAP32(bits);std::memcpy(out+k,&bits,4);}
        }else for(uint32_t k=0;k<count;++k)out[k]=comp(fmt,k,scale);
    }
    // All source color encodings expand to exact integers in [0,255].
    // The GPU byte layout can retain these directly instead of round-tripping
    // through eight floats, rounding and clamping them back to the same bytes.
    void color_bytes(uint32_t fmt,uint8_t out[4]) const {
        switch(fmt){
        case 0: {const uint16_t v=u16(0);out[0]=(v>>11)*255/31;out[1]=((v>>5)&63)*255/63;out[2]=(v&31)*255/31;out[3]=255;break;}
        case 1: case 2: out[0]=u8(0);out[1]=u8(1);out[2]=u8(2);out[3]=255;break;
        case 3: {const uint16_t v=u16(0);out[0]=(v>>12)*17;out[1]=((v>>8)&15)*17;out[2]=((v>>4)&15)*17;out[3]=(v&15)*17;break;}
        case 4: {const uint32_t v=uint32_t(u8(0))<<16|uint32_t(u8(1))<<8|u8(2);out[0]=((v>>18)&63)*255/63;out[1]=((v>>12)&63)*255/63;out[2]=((v>>6)&63)*255/63;out[3]=(v&63)*255/63;break;}
        default: out[0]=u8(0);out[1]=u8(1);out[2]=u8(2);out[3]=u8(3);break;
        }
    }
    void color(uint32_t fmt, float out[4]) const {
        switch (fmt) {
        case 0: { uint16_t v = u16(0); out[0] = float((v >> 11) * 255 / 31); out[1] = float(((v >> 5) & 63) * 255 / 63);
                  out[2] = float((v & 31) * 255 / 31); out[3] = 255; break; }
        case 1: case 2: out[0] = u8(0); out[1] = u8(1); out[2] = u8(2); out[3] = 255; break;
        case 3: { uint16_t v = u16(0); out[0] = float((v >> 12) * 17); out[1] = float(((v >> 8) & 15) * 17);
                  out[2] = float(((v >> 4) & 15) * 17); out[3] = float((v & 15) * 17); break; }
        case 4: { uint32_t v = uint32_t(u8(0)) << 16 | uint32_t(u8(1)) << 8 | u8(2);
                  out[0] = float(((v >> 18) & 63) * 255 / 63); out[1] = float(((v >> 12) & 63) * 255 / 63);
                  out[2] = float(((v >> 6) & 63) * 255 / 63); out[3] = float((v & 63) * 255 / 63); break; }
        default: out[0] = u8(0); out[1] = u8(1); out[2] = u8(2); out[3] = u8(3); break;
        }
    }
};

Reader indexed(int array, uint32_t index) {
    Reader r;
    r.mem = true;
    r.addr = (g.cp[0xA0 + array] & 0x03FFFFFF) + index * (g.cp[0xB0 + array] & 0xFF);
    // Array data lives in main RAM (physical addresses): read it through a host pointer instead of byte by byte
    // through the MMIO-checking accessors (same bytes; attributes are at most 36 bytes).
    if (r.addr < ssne::kRamSize - 64 && (r.addr >> 24) != 0xCC) { r.p = ssne::g_ram + r.addr; r.mem = false; }
    return r;
}

void mul34(uint32_t base, const float in[4], float out[3]) {
    for (int r = 0; r < 3; r++)
        out[r] = xf_f(base + r * 4) * in[0] + xf_f(base + r * 4 + 1) * in[1] + xf_f(base + r * 4 + 2) * in[2] +
                 xf_f(base + r * 4 + 3) * in[3];
}

float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

void light_channel(int chan, const float view_pos[3], const float nrm[3], const float vcol[2][4], bool has_vcol[2],
                   float out[4]) {
    for (int part = 0; part < 2; part++) {  // 0 = color (rgb), 1 = alpha
        uint32_t ctrl = g.xf_reg[(part ? 0x10 : 0x0E) + chan];
        uint32_t matreg = g.xf_reg[0x0C + chan], ambreg = g.xf_reg[0x0A + chan];
        float mat[4], amb[4];
        for (int k = 0; k < 4; k++) {
            mat[k] = float((matreg >> (24 - 8 * k)) & 0xFF);
            amb[k] = float((ambreg >> (24 - 8 * k)) & 0xFF);
        }
        if (ctrl & 1) for (int k = 0; k < 4; k++) mat[k] = has_vcol[chan] ? vcol[chan][k] : 255.f;
        if (ctrl & 0x40) for (int k = 0; k < 4; k++) amb[k] = has_vcol[chan] ? vcol[chan][k] : 255.f;
        float res[4];
        if (!(ctrl & 2)) {
            memcpy(res, mat, sizeof(res));
        } else {
            float lit[4] = {amb[0], amb[1], amb[2], amb[3]};
            uint32_t mask = ((ctrl >> 2) & 0xF) | (((ctrl >> 11) & 0xF) << 4);
            uint32_t diffuse_fn = (ctrl >> 7) & 3;
            bool atten = (ctrl >> 9) & 1, spot = (ctrl >> 10) & 1;
            for (int l = 0; l < 8; l++) {
                if (!(mask & (1u << l))) continue;
                uint32_t base = 0x600 + l * 0x10;
                uint32_t lc = g.xf_mem[base + 3];
                float lpos[3] = {xf_f(base + 10), xf_f(base + 11), xf_f(base + 12)};
                float ldir[3] = {xf_f(base + 13), xf_f(base + 14), xf_f(base + 15)};
                float L[3] = {lpos[0] - view_pos[0], lpos[1] - view_pos[1], lpos[2] - view_pos[2]};
                float dist2 = L[0] * L[0] + L[1] * L[1] + L[2] * L[2];
                float dist = std::sqrt(dist2);
                if (dist > 0) for (float& c : L) c /= dist;
                float ndl = nrm[0] * L[0] + nrm[1] * L[1] + nrm[2] * L[2];
                float diff = diffuse_fn == 0 ? 1.f : diffuse_fn == 1 ? ndl : std::max(0.f, ndl);
                float att = 1.f;
                if (atten) {
                    float a0 = xf_f(base + 4), a1 = xf_f(base + 5), a2 = xf_f(base + 6);
                    float k0 = xf_f(base + 7), k1 = xf_f(base + 8), k2 = xf_f(base + 9);
                    if (spot) {
                        float cosang = std::max(0.f, -(L[0] * ldir[0] + L[1] * ldir[1] + L[2] * ldir[2]));
                        float num = std::max(0.f, a0 + a1 * cosang + a2 * cosang * cosang);
                        float den = k0 + k1 * dist + k2 * dist2;
                        att = den > 0 ? num / den : num;
                    } else {  // specular: direction holds the half-angle vector
                        float h = nrm[0] * ldir[0] + nrm[1] * ldir[1] + nrm[2] * ldir[2];
                        float c = ndl > 0 ? std::max(0.f, h) : 0.f;
                        float num = std::max(0.f, a0 + a1 * c + a2 * c * c);
                        float den = k0 + k1 * c + k2 * c * c;
                        att = den > 0 ? num / den : num;
                    }
                }
                for (int k = 0; k < 4; k++) lit[k] += float((lc >> (24 - 8 * k)) & 0xFF) * diff * att;
            }
            for (int k = 0; k < 4; k++) res[k] = mat[k] * clamp01(lit[k] / 255.f);
        }
        if (part == 0) { out[0] = res[0]; out[1] = res[1]; out[2] = res[2]; }
        else out[3] = res[3];
    }
}

// Register decoding belongs to a draw, not to every vertex. Keep the
// reference shader above independent for bit-exact differential tests.
struct LightingPart {bool vertex_material,vertex_ambient,enabled,atten,spot;uint32_t mask,diffuse;};
struct LightingChannel {float material[4],ambient[4];LightingPart parts[2];};
struct LightingLight {float color[4],position[3],direction[3],a[3],k[3];};
struct LightGeometry {float direction[3],dist2,dist,ndl;};
struct DrawLighting {LightingChannel channels[2];LightingLight lights[8];uint32_t mask=0;bool normals=false;};
DrawLighting prepare_lighting(unsigned nchan){
    DrawLighting result;uint32_t used=0;
    for(unsigned c=0;c<std::max(nchan,1u);++c){auto& channel=result.channels[c];
        const auto material=g.xf_reg[0x0C+c],ambient=g.xf_reg[0x0A+c];
        for(unsigned k=0;k<4;++k)channel.material[k]=float((material>>(24-8*k))&255u);
        bool needs_ambient=false;
        for(unsigned part=0;part<2;++part){const auto ctrl=g.xf_reg[(part?0x10:0x0E)+c];auto& p=channel.parts[part];
            p.vertex_material=(ctrl&1)!=0;p.enabled=(ctrl&2)!=0;
            if(!p.enabled)continue;needs_ambient=true;p.vertex_ambient=(ctrl&0x40)!=0;
            p.mask=((ctrl>>2)&15u)|(((ctrl>>11)&15u)<<4);p.diffuse=(ctrl>>7)&3u;p.atten=(ctrl&(1u<<9))!=0;p.spot=(ctrl&(1u<<10))!=0;
            used|=p.mask;if(p.mask && (p.diffuse!=0 || (p.atten && !p.spot)))result.normals=true;
        }
        if(needs_ambient)for(unsigned k=0;k<4;++k)channel.ambient[k]=float((ambient>>(24-8*k))&255u);
    }
    result.mask=used;
    for(unsigned l=0;l<8;++l)if(used&(1u<<l)){auto& light=result.lights[l];const auto base=0x600+l*0x10,color=g.xf_mem[base+3];
        for(unsigned k=0;k<4;++k)light.color[k]=float((color>>(24-8*k))&255u);
        for(unsigned k=0;k<3;++k){light.position[k]=xf_f(base+10+k);light.direction[k]=xf_f(base+13+k);light.a[k]=xf_f(base+4+k);light.k[k]=xf_f(base+7+k);}
    }
    return result;
}
void light_channel_prepared(const DrawLighting& draw,const LightGeometry geometry[8],unsigned chan,const float nrm[3],const float vcol[2][4],const bool has_vcol[2],float out[4]){
    static constexpr float white[4]={255,255,255,255};const auto& channel=draw.channels[chan];
    for(unsigned part=0;part<2;++part){const auto& p=channel.parts[part];
        const auto* material=p.vertex_material?(has_vcol[chan]?vcol[chan]:white):channel.material;
        const unsigned first=part?3:0,last=part?4:3;
        if(!p.enabled){for(unsigned k=first;k<last;++k)out[k]=material[k];continue;}
        const auto* ambient=p.vertex_ambient?(has_vcol[chan]?vcol[chan]:white):channel.ambient;
        float lit[4];for(unsigned k=first;k<last;++k)lit[k]=ambient[k];
        for(unsigned l=0;l<8;++l){if(!(p.mask&(1u<<l)))continue;const auto& light=draw.lights[l];
            const auto& geo=geometry[l];const auto* L=geo.direction;
            const float dist2=geo.dist2,dist=geo.dist,ndl=geo.ndl;
            const float diff=p.diffuse==0?1.f:p.diffuse==1?ndl:std::max(0.f,ndl);float att=1.f;
            if(p.atten){
                if(p.spot){const float cosang=std::max(0.f,-(L[0]*light.direction[0]+L[1]*light.direction[1]+L[2]*light.direction[2]));
                    const float num=std::max(0.f,light.a[0]+light.a[1]*cosang+light.a[2]*cosang*cosang),den=light.k[0]+light.k[1]*dist+light.k[2]*dist2;att=den>0?num/den:num;
                }else{const float h=nrm[0]*light.direction[0]+nrm[1]*light.direction[1]+nrm[2]*light.direction[2],c=ndl>0?std::max(0.f,h):0.f;
                    const float num=std::max(0.f,light.a[0]+light.a[1]*c+light.a[2]*c*c),den=light.k[0]+light.k[1]*c+light.k[2]*c*c;att=den>0?num/den:num;
                }
            }
            // RGB and alpha use separate controls. Only these components
            // survive this part; preserve each component's arithmetic order.
            for(unsigned k=first;k<last;++k)lit[k]+=light.color[k]*diff*att;
        }
        for(unsigned k=first;k<last;++k)out[k]=material[k]*clamp01(lit[k]/255.f);
    }
}

} // namespace

uint32_t vertex_size(int vat) {
    uint32_t lo = g.cp[0x50], hi = g.cp[0x60];
    uint32_t a = vat_reg(vat, 0);
    uint32_t size = 0;
    for (int i = 0; i < 9; i++) size += (lo >> i) & 1;
    uint32_t pos = (lo >> 9) & 3;
    if (pos == 1) size += ((a & 1) ? 3 : 2) * comp_size((a >> 1) & 7);
    else size += index_size(pos);
    uint32_t nrm = (lo >> 11) & 3;
    if (nrm) {
        bool nbt = (a >> 9) & 1, idx3 = (a >> 31) & 1;
        if (nrm == 1) size += (nbt ? 9 : 3) * comp_size((a >> 10) & 7);
        else size += index_size(nrm) * ((nbt && idx3) ? 3 : 1);
    }
    for (int k = 0; k < 2; k++) {
        uint32_t col = (lo >> (13 + 2 * k)) & 3;
        uint32_t fmt = k == 0 ? (a >> 14) & 7 : (a >> 18) & 7;
        if (col == 1) size += color_size(fmt);
        else size += index_size(col);
    }
    for (int t = 0; t < 8; t++) {
        uint32_t tc = (hi >> (2 * t)) & 3;
        if (!tc) continue;
        uint32_t reg = vat_reg(vat, kTexVat[t].reg);
        if (tc == 1) size += (((reg >> kTexVat[t].cnt) & 1) ? 2 : 1) * comp_size((reg >> kTexVat[t].fmt) & 7);
        else size += index_size(tc);
    }
    return size;
}

void load_indexed_xf(int array, uint32_t v) {
    uint32_t index = v >> 16, count = ((v >> 12) & 0xF) + 1, addr = v & 0xFFF;
    uint32_t src = (g.cp[0xA0 + array] & 0x03FFFFFF) + index * (g.cp[0xB0 + array] & 0xFF);
    for (uint32_t k = 0; k < count; k++) g.xf_mem[(addr + k) & 0xFFF] = mem_read32(src + 4 * k);
}

// Reference implementation (decodes every register per vertex). Kept for SSNE_CHECK_VERTICES=1, which runs both
// decoders and reports any difference; decode_vertices() below is the fast one.
void decode_vertices_ref(int vat, const uint8_t* data, uint32_t count, std::vector<Vtx>& out) {
    out.resize(count);
    uint32_t lo = g.cp[0x50], hi = g.cp[0x60];
    uint32_t a = vat_reg(vat, 0);
    uint32_t vsize = vertex_size(vat);
    uint32_t mia = g.xf_reg[0x18], mib = g.xf_reg[0x19];
    float p0 = xfr_f(0x20), p1 = xfr_f(0x21), p2 = xfr_f(0x22), p3 = xfr_f(0x23), p4 = xfr_f(0x24), p5 = xfr_f(0x25);
    bool ortho = g.xf_reg[0x26] & 1;
    uint32_t ntexgen = std::min<uint32_t>(g.xf_reg[0x3F] & 0xF, 8);
    uint32_t nchan = std::min<uint32_t>(g.xf_reg[0x09] & 3, 2);

    for (uint32_t v = 0; v < count; v++) {
        const uint8_t* p = data + size_t(v) * vsize;
        uint32_t off = 0;
        uint32_t pos_idx = mia & 0x3F;
        uint32_t tex_idx[8];
        for (int t = 0; t < 8; t++) tex_idx[t] = t < 4 ? (mia >> (6 + 6 * t)) & 0x3F : (mib >> (6 * (t - 4))) & 0x3F;
        if (lo & 1) pos_idx = p[off++] & 0x3F;
        for (int t = 0; t < 8; t++) if ((lo >> (1 + t)) & 1) tex_idx[t] = p[off++] & 0x3F;

        auto attr_reader = [&](uint32_t type, int array, uint32_t direct_size) {
            Reader r;
            if (type == 1) { r.p = p + off; off += direct_size; }
            else if (type == 2) { r = indexed(array, p[off]); off += 1; }
            else { r = indexed(array, uint32_t(p[off]) << 8 | p[off + 1]); off += 2; }
            return r;
        };

        // position
        float pos[4] = {0, 0, 0, 1};
        uint32_t ptype = (lo >> 9) & 3;
        if (ptype) {
            uint32_t fmt = (a >> 1) & 7, n = (a & 1) ? 3 : 2;
            float scale = fmt == 4 ? 1.f : 1.f / float(1u << ((a >> 4) & 31));
            Reader r = attr_reader(ptype, 0, n * comp_size(fmt));
            for (uint32_t k = 0; k < n; k++) pos[k] = r.comp(fmt, k, scale);
        }
        // normal
        float nrm[3] = {0, 0, 1};
        uint32_t ntype = (lo >> 11) & 3;
        if (ntype) {
            uint32_t fmt = (a >> 10) & 7;
            bool nbt = (a >> 9) & 1, idx3 = (a >> 31) & 1;
            static const float nscale[5] = {1.f / 128, 1.f / 64, 1.f / 32768, 1.f / 16384, 1.f};
            float scale = nscale[std::min<uint32_t>(fmt, 4)];
            uint32_t n = nbt ? 9 : 3;
            Reader r = attr_reader(ntype, 1, n * comp_size(fmt));
            for (int k = 0; k < 3; k++) nrm[k] = r.comp(fmt, uint32_t(k), scale);
            if (ntype != 1 && nbt && idx3) off += 2 * index_size(ntype);  // B and T indices (unused)
        }
        // colors
        float vcol[2][4] = {};
        bool has_vcol[2] = {false, false};
        for (int c = 0; c < 2; c++) {
            uint32_t ctype = (lo >> (13 + 2 * c)) & 3;
            if (!ctype) continue;
            uint32_t fmt = c == 0 ? (a >> 14) & 7 : (a >> 18) & 7;
            Reader r = attr_reader(ctype, 2 + c, color_size(fmt));
            r.color(fmt, vcol[c]);
            has_vcol[c] = true;
        }
        // texcoords
        float tc[8][2] = {};
        for (int t = 0; t < 8; t++) {
            uint32_t ttype = (hi >> (2 * t)) & 3;
            if (!ttype) continue;
            uint32_t reg = vat_reg(vat, kTexVat[t].reg);
            uint32_t fmt = (reg >> kTexVat[t].fmt) & 7, n = ((reg >> kTexVat[t].cnt) & 1) ? 2 : 1;
            float scale = fmt == 4 ? 1.f : 1.f / float(1u << tex_shift(vat, t));
            Reader r = attr_reader(ttype, 4 + t, n * comp_size(fmt));
            for (uint32_t k = 0; k < n; k++) tc[t][k] = r.comp(fmt, k, scale);
        }

        // ---- transform
        Vtx& o = out[v];
        float view[3];
        mul34(pos_idx * 4, pos, view);
        float vn[3] = {0, 0, 1};
        {
            uint32_t nb = 0x400 + (pos_idx & 31) * 3;
            for (int r = 0; r < 3; r++)
                vn[r] = xf_f(nb + r * 3) * nrm[0] + xf_f(nb + r * 3 + 1) * nrm[1] + xf_f(nb + r * 3 + 2) * nrm[2];
            float len = std::sqrt(vn[0] * vn[0] + vn[1] * vn[1] + vn[2] * vn[2]);
            if (len > 0) for (float& c : vn) c /= len;
        }
        if (ortho) { o.x = p0 * view[0] + p1; o.y = p2 * view[1] + p3; o.z = p4 * view[2] + p5; o.w = 1; }
        else { o.x = p0 * view[0] + p1 * view[2]; o.y = p2 * view[1] + p3 * view[2]; o.z = p4 * view[2] + p5; o.w = -view[2]; }

        for (uint32_t c = 0; c < 2; c++) {
            if (c < nchan || c == 0) light_channel(int(c), view, vn, vcol, has_vcol, o.color[c]);
            else for (int k = 0; k < 4; k++) o.color[c][k] = 0;
        }

        for (uint32_t t = 0; t < 8; t++) {
            o.tex[t][0] = o.tex[t][1] = 0; o.tex[t][2] = 1;
            if (t >= ntexgen) continue;
            uint32_t tg = g.xf_reg[0x40 + t];
            bool stq = (tg >> 1) & 1, abc1 = (tg >> 2) & 1;
            uint32_t type = (tg >> 4) & 7, src = (tg >> 7) & 31;
            float in[4] = {0, 0, 1, 1};
            if (src == 0) { in[0] = pos[0]; in[1] = pos[1]; in[2] = pos[2]; }
            else if (src == 1) { in[0] = nrm[0]; in[1] = nrm[1]; in[2] = nrm[2]; }
            else if (src >= 5 && src <= 12) { in[0] = tc[src - 5][0]; in[1] = tc[src - 5][1]; }
            if (!abc1) in[2] = 1.f;
            float s = 0, tt = 0, q = 1;
            if (type == 0) {
                uint32_t base = tex_idx[t] * 4;
                s = xf_f(base) * in[0] + xf_f(base + 1) * in[1] + xf_f(base + 2) * in[2] + xf_f(base + 3) * in[3];
                tt = xf_f(base + 4) * in[0] + xf_f(base + 5) * in[1] + xf_f(base + 6) * in[2] + xf_f(base + 7) * in[3];
                if (stq) q = xf_f(base + 8) * in[0] + xf_f(base + 9) * in[1] + xf_f(base + 10) * in[2] + xf_f(base + 11) * in[3];
            } else if (type == 1) {  // emboss: reuse the source texgen (bump offset ignored)
                uint32_t es = (tg >> 12) & 7;
                s = o.tex[es][0]; tt = o.tex[es][1]; q = o.tex[es][2];
            } else {                 // color -> texcoord
                s = o.color[0][0] / 255.f; tt = o.color[0][1] / 255.f;
            }
            if (g.xf_reg[0x12] & 1) {  // dual texture transform (post matrix)
                uint32_t pt = g.xf_reg[0x50 + t];
                if ((pt >> 8) & 1) {
                    float len = std::sqrt(s * s + tt * tt + q * q);
                    if (len > 0) { s /= len; tt /= len; q /= len; }
                }
                uint32_t base = 0x500 + (pt & 0x3F) * 4;
                float in2[4] = {s, tt, q, 1};
                float r3[3];
                mul34(base, in2, r3);
                s = r3[0]; tt = r3[1]; q = stq ? r3[2] : 1.f;
            }
            o.tex[t][0] = s; o.tex[t][1] = tt; o.tex[t][2] = q;
        }
    }
}

namespace {

// Per-call attribute layout: everything decode_vertices_ref re-derives from the CP/XF registers for every vertex.
struct AttrDesc { uint32_t type = 0, fmt = 0, n = 0, direct = 0; float scale = 1; int array = 0; };

inline Reader attr_at(const AttrDesc& d, const uint8_t* p, uint32_t& off) {
    Reader r;
    if (d.type == 1) { r.p = p + off; off += d.direct; }
    else if (d.type == 2) { r = indexed(d.array, p[off]); off += 1; }
    else { r = indexed(d.array, uint32_t(p[off]) << 8 | p[off + 1]); off += 2; }
    return r;
}

} // namespace

static void decode_vertices_impl(int vat, const uint8_t* data, uint32_t count, std::vector<Vtx>& out, bool allow_gpu) {
    out.resize(count);
    uint32_t lo = g.cp[0x50], hi = g.cp[0x60];
    uint32_t a = vat_reg(vat, 0);
    uint32_t vsize = vertex_size(vat);
    uint32_t mia = g.xf_reg[0x18], mib = g.xf_reg[0x19];
    float p0 = xfr_f(0x20), p1 = xfr_f(0x21), p2 = xfr_f(0x22), p3 = xfr_f(0x23), p4 = xfr_f(0x24), p5 = xfr_f(0x25);
    bool ortho = g.xf_reg[0x26] & 1;
    uint32_t ntexgen = std::min<uint32_t>(g.xf_reg[0x3F] & 0xF, 8);
    uint32_t nchan = std::min<uint32_t>(g.xf_reg[0x09] & 3, 2);

    uint32_t def_tex_idx[8];
    for (int t = 0; t < 8; t++) def_tex_idx[t] = t < 4 ? (mia >> (6 + 6 * t)) & 0x3F : (mib >> (6 * (t - 4))) & 0x3F;
    uint32_t tex_mi_mask = (lo >> 1) & 0xFF;  // which texcoords carry a per-vertex matrix index byte
    AttrDesc pos_d, nrm_d, col_d[2], tex_d[8];
    pos_d.type = (lo >> 9) & 3;
    if (pos_d.type) {
        pos_d.fmt = (a >> 1) & 7; pos_d.n = (a & 1) ? 3 : 2;
        pos_d.scale = pos_d.fmt == 4 ? 1.f : 1.f / float(1u << ((a >> 4) & 31));
        pos_d.direct = pos_d.n * uint32_t(comp_size(pos_d.fmt)); pos_d.array = 0;
    }
    nrm_d.type = (lo >> 11) & 3;
    bool nbt = (a >> 9) & 1, idx3 = (a >> 31) & 1;
    uint32_t nrm_skip = 0;
    if (nrm_d.type) {
        static const float nscale[5] = {1.f / 128, 1.f / 64, 1.f / 32768, 1.f / 16384, 1.f};
        nrm_d.fmt = (a >> 10) & 7;
        nrm_d.scale = nscale[std::min<uint32_t>(nrm_d.fmt, 4)];
        nrm_d.direct = (nbt ? 9 : 3) * uint32_t(comp_size(nrm_d.fmt)); nrm_d.array = 1;
        if (nrm_d.type != 1 && nbt && idx3) nrm_skip = 2 * uint32_t(index_size(nrm_d.type));
    }
    for (int c = 0; c < 2; c++) {
        col_d[c].type = (lo >> (13 + 2 * c)) & 3;
        col_d[c].fmt = c == 0 ? (a >> 14) & 7 : (a >> 18) & 7;
        col_d[c].direct = uint32_t(color_size(col_d[c].fmt)); col_d[c].array = 2 + c;
    }
    for (int t = 0; t < 8; t++) {
        AttrDesc& d = tex_d[t];
        d.type = (hi >> (2 * t)) & 3;
        if (!d.type) continue;
        uint32_t reg = vat_reg(vat, kTexVat[t].reg);
        d.fmt = (reg >> kTexVat[t].fmt) & 7; d.n = ((reg >> kTexVat[t].cnt) & 1) ? 2 : 1;
        d.scale = d.fmt == 4 ? 1.f : 1.f / float(1u << tex_shift(vat, t));
        d.direct = d.n * uint32_t(comp_size(d.fmt)); d.array = 4 + t;
    }
    // texgen setup
    struct TexGen { bool stq, abc1, dual_norm; uint32_t type, src, es, post_base; };
    TexGen tgs[8];
    bool dual = g.xf_reg[0x12] & 1;
    for (uint32_t t = 0; t < ntexgen; t++) {
        uint32_t tg = g.xf_reg[0x40 + t];
        TexGen& x = tgs[t];
        x.stq = (tg >> 1) & 1; x.abc1 = (tg >> 2) & 1; x.type = (tg >> 4) & 7; x.src = (tg >> 7) & 31; x.es = (tg >> 12) & 7;
        uint32_t pt = g.xf_reg[0x50 + t];
        x.dual_norm = (pt >> 8) & 1;
        x.post_base = 0x500 + (pt & 0x3F) * 4;
    }
    bool gpu_xf = allow_gpu && !ortho && count >= 3;
    // Preserve undefined forward/self emboss dependencies on the reference path.
    for(unsigned t=0;t<ntexgen;++t)if(tgs[t].type==1 && tgs[t].es>=t)gpu_xf=false;
    if (gpu_xf) {
        // GPU draws only need raw attributes. Prepare stream offsets once, and
        // avoid the CPU lighting/texgen loop and its per-vertex scratch arrays.
        // This plan is local to this draw: indexed RAM is still read afresh for
        // every vertex, including writes between repeated display-list calls.
        uint32_t offset = (lo & 1) ? 1 : 0;
        int matrix_offsets[8];
        for (unsigned t=0;t<8;++t) {
            matrix_offsets[t] = (tex_mi_mask & (1u<<t)) ? int(offset++) : -1;
        }
        auto stream_offset = [&](const AttrDesc& d) {
            const auto result=offset;
            if(d.type)offset+=d.type==1?d.direct:uint32_t(index_size(d.type));
            return result;
        };
        const auto pos_offset=stream_offset(pos_d), normal_offset=stream_offset(nrm_d);
        offset+=nrm_skip;
        uint32_t color_offsets[2],tex_offsets[8];
        for(unsigned c=0;c<2;++c)color_offsets[c]=stream_offset(col_d[c]);
        unsigned active_tex[8],active_count=0;
        for(unsigned t=0;t<8;++t){
            tex_offsets[t]=stream_offset(tex_d[t]);
            if(tex_d[t].type)active_tex[active_count++]=t;
        }
        auto read = [](const AttrDesc& d,const uint8_t* p,uint32_t at) {
            return attr_at(d,p,at);
        };
        for(uint32_t v=0;v<count;++v){
            const auto* p=data+size_t(v)*vsize;
            auto& o=out[v];
            o.gpu_xf=true;o.position_matrix=uint8_t((lo&1)?p[0]&63:mia&63);
            float pos[3]={0,0,0};
            if(pos_d.type){const auto r=read(pos_d,p,pos_offset);
                for(unsigned k=0;k<pos_d.n;++k)pos[k]=r.comp(pos_d.fmt,k,pos_d.scale);}
            o.x=pos[0];o.y=pos[1];o.z=pos[2];o.w=1;
            o.raw_normal[0]=o.raw_normal[1]=0;o.raw_normal[2]=1;
            if(nrm_d.type){const auto r=read(nrm_d,p,normal_offset);
                for(unsigned k=0;k<3;++k)o.raw_normal[k]=r.comp(nrm_d.fmt,k,nrm_d.scale);}
            for(unsigned c=0;c<2;++c){
                if(col_d[c].type)read(col_d[c],p,color_offsets[c]).color(col_d[c].fmt,o.color[c]);
                else for(float& value:o.color[c])value=255.f;
            }
            for(unsigned t=0;t<8;++t){
                o.tex[t][0]=o.tex[t][1]=0;
                o.tex[t][2]=float(matrix_offsets[t]>=0?p[matrix_offsets[t]]&63:def_tex_idx[t]);
            }
            for(unsigned a=0;a<active_count;++a){
                const auto t=active_tex[a];const auto& d=tex_d[t];const auto r=read(d,p,tex_offsets[t]);
                for(unsigned k=0;k<d.n;++k)o.tex[t][k]=r.comp(d.fmt,k,d.scale);
            }
        }
        return;
    }
    const auto lighting=gpu_xf ? DrawLighting{} : prepare_lighting(nchan);

    for (uint32_t v = 0; v < count; v++) {
        const uint8_t* p = data + size_t(v) * vsize;
        uint32_t off = 0;
        uint32_t pos_idx = mia & 0x3F;
        uint32_t tex_idx[8];
        memcpy(tex_idx, def_tex_idx, sizeof(tex_idx));
        if (lo & 1) pos_idx = p[off++] & 0x3F;
        if (tex_mi_mask)
            for (int t = 0; t < 8; t++) if ((tex_mi_mask >> t) & 1) tex_idx[t] = p[off++] & 0x3F;

        float pos[4] = {0, 0, 0, 1};
        if (pos_d.type) {
            Reader r = attr_at(pos_d, p, off);
            for (uint32_t k = 0; k < pos_d.n; k++) pos[k] = r.comp(pos_d.fmt, k, pos_d.scale);
        }
        float nrm[3] = {0, 0, 1};
        if (nrm_d.type) {
            Reader r = attr_at(nrm_d, p, off);
            for (int k = 0; k < 3; k++) nrm[k] = r.comp(nrm_d.fmt, uint32_t(k), nrm_d.scale);
            off += nrm_skip;
        }
        float vcol[2][4] = {};
        bool has_vcol[2] = {false, false};
        for (int c = 0; c < 2; c++) {
            if (!col_d[c].type) continue;
            Reader r = attr_at(col_d[c], p, off);
            r.color(col_d[c].fmt, vcol[c]);
            has_vcol[c] = true;
        }
        float tc[8][2] = {};
        for (int t = 0; t < 8; t++) {
            const AttrDesc& d = tex_d[t];
            if (!d.type) continue;
            Reader r = attr_at(d, p, off);
            for (uint32_t k = 0; k < d.n; k++) tc[t][k] = r.comp(d.fmt, k, d.scale);
        }

        // ---- transform (same arithmetic as decode_vertices_ref)
        Vtx& o = out[v];
        o.gpu_xf=gpu_xf;o.position_matrix=gpu_xf?uint8_t(pos_idx):0;
        for(unsigned k=0;k<3;++k)o.raw_normal[k]=gpu_xf?nrm[k]:0.f;
        float view[3]{};
        if(!gpu_xf)mul34(pos_idx * 4, pos, view);
        float vn[3] = {0, 0, 1};
        if(lighting.normals) {
            uint32_t nb = 0x400 + (pos_idx & 31) * 3;
            for (int r = 0; r < 3; r++)
                vn[r] = xf_f(nb + r * 3) * nrm[0] + xf_f(nb + r * 3 + 1) * nrm[1] + xf_f(nb + r * 3 + 2) * nrm[2];
            float len = std::sqrt(vn[0] * vn[0] + vn[1] * vn[1] + vn[2] * vn[2]);
            if (len > 0) for (float& c : vn) c /= len;
        }
        if(gpu_xf){o.x=pos[0];o.y=pos[1];o.z=pos[2];o.w=1;}
        else if (ortho) { o.x = p0 * view[0] + p1; o.y = p2 * view[1] + p3; o.z = p4 * view[2] + p5; o.w = 1; }
        else { o.x = p0 * view[0] + p1 * view[2]; o.y = p2 * view[1] + p3 * view[2]; o.z = p4 * view[2] + p5; o.w = -view[2]; }

        LightGeometry geometry[8];
        // Distance, normalized light direction and N dot L are identical for
        // RGB, alpha and both color channels. Evaluate each used light once.
        if(lighting.mask)for(unsigned l=0;l<8;++l)if(lighting.mask&(1u<<l)){
            auto& geo=geometry[l];const auto& light=lighting.lights[l];
            for(unsigned k=0;k<3;++k)geo.direction[k]=light.position[k]-view[k];
            auto* L=geo.direction;geo.dist2=L[0]*L[0]+L[1]*L[1]+L[2]*L[2];geo.dist=std::sqrt(geo.dist2);
            if(geo.dist>0)for(float& c:geo.direction)c/=geo.dist;
            geo.ndl=vn[0]*L[0]+vn[1]*L[1]+vn[2]*L[2];
        }

        for (uint32_t c = 0; c < 2; c++) {
            if(gpu_xf){for(unsigned k=0;k<4;++k)o.color[c][k]=has_vcol[c]?vcol[c][k]:255.f;continue;}
            if(c<nchan || c==0){
                const auto& channel=lighting.channels[c];
                if(!channel.parts[0].enabled && !channel.parts[1].enabled){
                    for(unsigned k=0;k<3;++k)o.color[c][k]=channel.parts[0].vertex_material?(has_vcol[c]?vcol[c][k]:255.f):channel.material[k];
                    o.color[c][3]=channel.parts[1].vertex_material?(has_vcol[c]?vcol[c][3]:255.f):channel.material[3];
                }else light_channel_prepared(lighting,geometry,c,vn,vcol,has_vcol,o.color[c]);
            }else for(int k=0;k<4;++k)o.color[c][k]=0;
        }

        for (uint32_t t = 0; t < 8; t++) {
            if(gpu_xf){o.tex[t][0]=tc[t][0];o.tex[t][1]=tc[t][1];o.tex[t][2]=float(tex_idx[t]);continue;}
            o.tex[t][0] = o.tex[t][1] = 0; o.tex[t][2] = 1;
            if (t >= ntexgen) continue;
            const TexGen& x = tgs[t];
            float in[4] = {0, 0, 1, 1};
            if (x.src == 0) { in[0] = pos[0]; in[1] = pos[1]; in[2] = pos[2]; }
            else if (x.src == 1) { in[0] = nrm[0]; in[1] = nrm[1]; in[2] = nrm[2]; }
            else if (x.src >= 5 && x.src <= 12) { in[0] = tc[x.src - 5][0]; in[1] = tc[x.src - 5][1]; }
            if (!x.abc1) in[2] = 1.f;
            float s = 0, tt = 0, q = 1;
            if (x.type == 0) {
                uint32_t base = tex_idx[t] * 4;
                s = xf_f(base) * in[0] + xf_f(base + 1) * in[1] + xf_f(base + 2) * in[2] + xf_f(base + 3) * in[3];
                tt = xf_f(base + 4) * in[0] + xf_f(base + 5) * in[1] + xf_f(base + 6) * in[2] + xf_f(base + 7) * in[3];
                if (x.stq) q = xf_f(base + 8) * in[0] + xf_f(base + 9) * in[1] + xf_f(base + 10) * in[2] + xf_f(base + 11) * in[3];
            } else if (x.type == 1) {
                s = o.tex[x.es][0]; tt = o.tex[x.es][1]; q = o.tex[x.es][2];
            } else {
                s = o.color[0][0] / 255.f; tt = o.color[0][1] / 255.f;
            }
            if (dual) {
                if (x.dual_norm) {
                    float len = std::sqrt(s * s + tt * tt + q * q);
                    if (len > 0) { s /= len; tt /= len; q /= len; }
                }
                float in2[4] = {s, tt, q, 1};
                float r3[3];
                mul34(x.post_base, in2, r3);
                s = r3[0]; tt = r3[1]; q = x.stq ? r3[2] : 1.f;
            }
            o.tex[t][0] = s; o.tex[t][1] = tt; o.tex[t][2] = q;
        }
    }
}

uint32_t gpu_packed_stride(uint32_t count){
    static const bool disabled=getenv("SSNE_CHECK_VERTICES") || getenv("SSNE_CPU_XF") || getenv("SSNE_GPU_PACKED_DISABLE") || getenv("SSNE_NORENDER");
    if(disabled || count<3 || (g.xf_reg[0x26]&1))return 0;
    const unsigned ntex=std::min(g.xf_reg[0x3F]&15u,8u);unsigned inputs=ntex;
    for(unsigned t=0;t<ntex;++t){const auto tg=g.xf_reg[0x40+t],type=(tg>>4)&7,src=(tg>>7)&31;
        if(type==1 && ((tg>>12)&7)>=t)return 0;
        if(type==0 && src>=5 && src<=12)inputs=std::max(inputs,src-4);}
    return 52+12*inputs;
}

bool gpu_packed_ram_only(int vat){
    // The direct block path cannot include memory-mapped reads or callbacks.
    // Prove every possible index uses the same direct-RAM Reader branch.
    const auto lo=g.cp[0x50],hi=g.cp[0x60];
    auto safe=[&](unsigned array,unsigned type){
        if(type<2)return true;
        const uint64_t base=g.cp[0xA0+array]&0x03ffffffu,step=g.cp[0xB0+array]&255u;
        return base+uint64_t(type==2?255u:65535u)*step<ssne::kRamSize-64u;
    };
    if(!safe(0,(lo>>9)&3) || !safe(1,(lo>>11)&3) || !safe(2,(lo>>13)&3) || !safe(3,(lo>>15)&3))return false;
    for(unsigned t=0;t<8;++t)if(!safe(4+t,(hi>>(2*t))&3))return false;
    return true;
}

bool prepare_gpu_common_plan(int vat,uint32_t stride,PackedCommonPlan& out){
    if(stride!=64 && stride!=76)return false;
    const auto lo=g.cp[0x50],hi=g.cp[0x60],a=vat_reg(vat,0);
    constexpr uint32_t mask=1u|(7u<<1)|(1u<<9)|(7u<<10)|(7u<<14)|(1u<<21)|(7u<<22);
    constexpr uint32_t value=1u|(4u<<1)|(4u<<10)|(5u<<14)|(1u<<21)|(4u<<22);
    if((lo!=0x5E00u && lo!=0x7E00u) || hi!=3u || (a&mask)!=value)return false;
    constexpr unsigned arrays[4]={0,1,2,4};
    PackedCommonPlan plan;
    plan.color16=lo==0x7E00u;
    for(unsigned k=0;k<4;++k){
        plan.base[k]=g.cp[0xA0+arrays[k]]&0x03FFFFFFu;
        plan.step[k]=g.cp[0xB0+arrays[k]]&255u;
        if(uint64_t(plan.base[k])+uint64_t(k==2&&!plan.color16?255u:65535u)*plan.step[k]>=ssne::kRamSize-64u)return false;
    }
    plan.input_stride=plan.color16?8:7;plan.uv_offset=plan.color16?6:5;
    plan.stride=stride;plan.inputs=(stride-52)/12;
    const auto mia=g.xf_reg[0x18];plan.position_matrix=float(mia&63u);
    for(unsigned k=0;k<2;++k){const float v=float((mia>>(6+6*k))&63u);std::memcpy(plan.tex_matrix+k,&v,4);}
    out=plan;return true;
}

static void decode_common(PackedCommonPlan plan,const uint8_t* data,uint32_t count,uint8_t* dest){
    constexpr uint32_t one=0x3F800000u,white=0xFFFFFFFFu;
    const uint32_t second_uv[3]={0,0,plan.tex_matrix[1]};const auto* ram=ssne::g_ram;
    for(unsigned v=0;v<count;++v,data+=plan.input_stride,dest+=plan.stride){
        const uint32_t indices[4]={uint32_t(data[0])<<8|data[1],uint32_t(data[2])<<8|data[3],plan.color16?(uint32_t(data[4])<<8|data[5]):data[4],uint32_t(data[plan.uv_offset])<<8|data[plan.uv_offset+1]};
        uint32_t position[4],normal[3],uv[3];position[3]=one;uv[2]=plan.tex_matrix[0];
        std::memcpy(position,ram+plan.base[0]+indices[0]*plan.step[0],12);
        std::memcpy(normal,ram+plan.base[1]+indices[1]*plan.step[1],12);
        std::memcpy(uv,ram+plan.base[3]+indices[3]*plan.step[3],8);
        for(unsigned k=0;k<3;++k){position[k]=SSNE_BSWAP32(position[k]);normal[k]=SSNE_BSWAP32(normal[k]);}
        uv[0]=SSNE_BSWAP32(uv[0]);uv[1]=SSNE_BSWAP32(uv[1]);
        std::memcpy(dest,position,16);std::memcpy(dest+16,ram+plan.base[2]+indices[2]*plan.step[2],4);std::memcpy(dest+20,&white,4);
        std::memcpy(dest+24,uv,12);if(plan.inputs==2)std::memcpy(dest+36,second_uv,12);
        std::memcpy(dest+24+12*plan.inputs,normal,12);std::memcpy(dest+36+12*plan.inputs,position,12);
        std::memcpy(dest+48+12*plan.inputs,&plan.position_matrix,4);
    }
}

bool decode_gpu_common_into(const PackedCommonPlan& plan,const uint8_t* data,uint32_t count,uint8_t* destination,size_t size){
    if(count<3 || size_t(count)*plan.stride!=size)return false;
    ssne::perf::detail::Scope timer(ssne::perf::detail::VertexDecode);
    ssne::perf::detail::count(ssne::perf::detail::Vertices,count);
    decode_common(plan,data,count,destination);return true;
}

bool prepare_gpu_vertex_plan(int vat,uint32_t packed_stride,PackedVertexPlan& out){
    if(packed_stride!=gpu_packed_stride(3) || !gpu_packed_ram_only(vat))return false;
    PackedVertexPlan plan{};plan.stride=packed_stride;plan.inputs=(packed_stride-52)/12;plan.input_stride=vertex_size(vat);
    plan.common_format=prepare_gpu_common_plan(vat,packed_stride,plan.common);
    if(plan.common_format){out=plan;return true;}
    const auto lo=g.cp[0x50],hi=g.cp[0x60],a=vat_reg(vat,0);
    const auto mia=g.xf_reg[0x18],mib=g.xf_reg[0x19],tex_mi_mask=(lo>>1)&255;
    plan.position_matrix=mia&63;plan.position_matrix_stream=lo&1;
    AttrDesc pos_d, nrm_d, col_d[2], tex_d[8];
    pos_d.type = (lo >> 9) & 3;
    if (pos_d.type) {
        pos_d.fmt = (a >> 1) & 7; pos_d.n = (a & 1) ? 3 : 2;
        pos_d.scale = pos_d.fmt == 4 ? 1.f : 1.f / float(1u << ((a >> 4) & 31));
        pos_d.direct = pos_d.n * uint32_t(comp_size(pos_d.fmt)); pos_d.array = 0;
    }
    nrm_d.type = (lo >> 11) & 3;
    bool nbt = (a >> 9) & 1, idx3 = (a >> 31) & 1;
    uint32_t nrm_skip = 0;
    if (nrm_d.type) {
        static const float nscale[5] = {1.f / 128, 1.f / 64, 1.f / 32768, 1.f / 16384, 1.f};
        nrm_d.fmt = (a >> 10) & 7;
        nrm_d.scale = nscale[std::min<uint32_t>(nrm_d.fmt, 4)];
        nrm_d.direct = (nbt ? 9 : 3) * uint32_t(comp_size(nrm_d.fmt)); nrm_d.array = 1;
        if (nrm_d.type != 1 && nbt && idx3) nrm_skip = 2 * uint32_t(index_size(nrm_d.type));
    }
    for (int c = 0; c < 2; c++) {
        col_d[c].type = (lo >> (13 + 2 * c)) & 3;
        col_d[c].fmt = c == 0 ? (a >> 14) & 7 : (a >> 18) & 7;
        col_d[c].direct = uint32_t(color_size(col_d[c].fmt)); col_d[c].array = 2 + c;
    }
    for (int t = 0; t < 8; t++) {
        AttrDesc& d = tex_d[t];
        d.type = (hi >> (2 * t)) & 3;
        if (!d.type) continue;
        uint32_t reg = vat_reg(vat, kTexVat[t].reg);
        d.fmt = (reg >> kTexVat[t].fmt) & 7; d.n = ((reg >> kTexVat[t].cnt) & 1) ? 2 : 1;
        d.scale = d.fmt == 4 ? 1.f : 1.f / float(1u << tex_shift(vat, t));
        d.direct = d.n * uint32_t(comp_size(d.fmt)); d.array = 4 + t;
    }
        uint32_t offset = (lo & 1) ? 1 : 0;
        int matrix_offsets[8];
        for (unsigned t=0;t<8;++t) {
            matrix_offsets[t] = (tex_mi_mask & (1u<<t)) ? int(offset++) : -1;
        }
        auto stream_offset = [&](const AttrDesc& d) {
            const auto result=offset;
            if(d.type)offset+=d.type==1?d.direct:uint32_t(index_size(d.type));
            return result;
        };
        const auto pos_offset=stream_offset(pos_d), normal_offset=stream_offset(nrm_d);
        offset+=nrm_skip;
        uint32_t color_offsets[2],tex_offsets[8];
        for(unsigned c=0;c<2;++c)color_offsets[c]=stream_offset(col_d[c]);
        unsigned active_tex[8],active_count=0;
        for(unsigned t=0;t<8;++t){
            tex_offsets[t]=stream_offset(tex_d[t]);
            if(tex_d[t].type)active_tex[active_count++]=t;
        }


    auto attribute=[](const AttrDesc& d,uint32_t offset){return PackedVertexPlan::Attribute{d.type,d.fmt,d.n,offset,uint32_t(d.array),d.scale};};
    plan.attributes[0]=attribute(pos_d,pos_offset);plan.attributes[1]=attribute(nrm_d,normal_offset);
    for(unsigned k=0;k<2;++k)plan.attributes[2+k]=attribute(col_d[k],color_offsets[k]);
    for(unsigned k=0;k<8;++k){plan.attributes[4+k]=attribute(tex_d[k],tex_offsets[k]);plan.tex_matrix_offset[k]=matrix_offsets[k];
        const float value=float(k<4?(mia>>(6+6*k))&63:(mib>>(6*(k-4)))&63);std::memcpy(plan.tex_matrix+k,&value,4);}
    for(unsigned k=0;k<12;++k){plan.base[k]=g.cp[0xA0+k]&0x03FFFFFFu;plan.step[k]=g.cp[0xB0+k]&255u;}
    for(unsigned k=0;k<12;++k){
        if(k==2||k==3||k>=4+plan.inputs)continue;
        if(plan.attributes[k].type&&plan.attributes[k].fmt<4)plan.quantized_components=true;
    }
    out=plan;return true;
}

namespace {
std::array<PackedVertexPlan,8> retained_vertex_plans;
uint8_t retained_vertex_plan_valid=0;
}
void invalidate_gpu_vertex_plans(){retained_vertex_plan_valid=0;}
const PackedVertexPlan* cached_gpu_vertex_plan(int vat,uint32_t stride){
    const auto bit=uint8_t(1u<<vat);auto& plan=retained_vertex_plans[vat];
    if(!(retained_vertex_plan_valid&bit) || plan.stride!=stride){
        if(!prepare_gpu_vertex_plan(vat,stride,plan))return nullptr;
        retained_vertex_plan_valid|=bit;
    }
    return &plan;
}

// Prepared plans prove these attribute bytes are direct FIFO/RAM input.
// Do not route each quantized scalar through the checked-memory Reader ABI.
SSNE_NOINLINE void direct_quantized_components(const uint8_t* p,uint32_t fmt,uint32_t count,float scale,float* out){
    switch(fmt){
    case 0: for(uint32_t k=0;k<count;++k)out[k]=p[k]*scale;break;
    case 1: for(uint32_t k=0;k<count;++k)out[k]=int8_t(p[k])*scale;break;
    case 2: for(uint32_t k=0;k<count;++k){uint16_t bits;std::memcpy(&bits,p+k*2,2);out[k]=SSNE_BSWAP16(bits)*scale;}break;
    case 3: for(uint32_t k=0;k<count;++k){uint16_t bits;std::memcpy(&bits,p+k*2,2);out[k]=int16_t(SSNE_BSWAP16(bits))*scale;}break;
    }
}
inline void direct_components(uint32_t fmt,uint32_t count,float scale,const uint8_t* p,float* out){
    if(fmt>=4){for(uint32_t k=0;k<count;++k){uint32_t bits;std::memcpy(&bits,p+k*4,4);bits=SSNE_BSWAP32(bits);std::memcpy(out+k,&bits,4);}}
    else direct_quantized_components(p,fmt,count,scale,out);
}
SSNE_NOINLINE bool decode_gpu_quantized_vertex_plan_into(const PackedVertexPlan& plan,const uint8_t* data,uint32_t count,uint8_t* dest,size_t size){
    if(count<3 || size_t(count)*plan.stride!=size)return false;
    if(plan.common_format)return decode_gpu_common_into(plan.common,data,count,dest,size);
    ssne::perf::detail::Scope timer(ssne::perf::detail::VertexDecode);
    ssne::perf::detail::count(ssne::perf::detail::Vertices,count);
    const auto inputs=plan.inputs;const auto* ram=ssne::g_ram;
    auto read=[&](const PackedVertexPlan::Attribute& d,const uint8_t* p){
        if(d.type==1)return p+d.offset;
        const auto index=d.type==2?p[d.offset]:(uint32_t(p[d.offset])<<8|p[d.offset+1]);
        return ram+plan.base[d.array]+index*plan.step[d.array];};
    for(unsigned v=0;v<count;++v,data+=plan.input_stride,dest+=plan.stride){
        const auto matrix=plan.position_matrix_stream?data[0]&63:plan.position_matrix;
        const auto& pos_d=plan.attributes[0];const auto& nrm_d=plan.attributes[1];
        float pos[4]={0,0,0,1};if(pos_d.type)direct_components(pos_d.fmt,pos_d.n,pos_d.scale,read(pos_d,data),pos);
        std::memcpy(dest,pos,16);std::memcpy(dest+36+12*inputs,pos,12);const float position_index=float(matrix);std::memcpy(dest+48+12*inputs,&position_index,4);
        float normal[3]={0,0,1};if(nrm_d.type)direct_components(nrm_d.fmt,3,nrm_d.scale,read(nrm_d,data),normal);
        std::memcpy(dest+24+12*inputs,normal,12);
        for(unsigned k=0;k<2;++k){const auto& d=plan.attributes[2+k];if(d.type)Reader{read(d,data)}.color_bytes(d.fmt,dest+16+k*4);else std::memset(dest+16+k*4,255,4);}
        for(unsigned k=0;k<inputs;++k){const auto& d=plan.attributes[4+k];float tc[3]={0,0,0};
            if(plan.tex_matrix_offset[k]>=0)tc[2]=float(data[plan.tex_matrix_offset[k]]&63);else std::memcpy(tc+2,plan.tex_matrix+k,4);
            if(d.type)direct_components(d.fmt,d.n,d.scale,read(d,data),tc);std::memcpy(dest+24+k*12,tc,12);
        }
    }
    return true;
}

SSNE_NOINLINE bool decode_gpu_float_vertex_plan_into(const PackedVertexPlan& plan,const uint8_t* data,uint32_t count,uint8_t* dest,size_t size){
    if(count<3 || size_t(count)*plan.stride!=size)return false;
    if(plan.common_format)return decode_gpu_common_into(plan.common,data,count,dest,size);
    ssne::perf::detail::Scope timer(ssne::perf::detail::VertexDecode);
    ssne::perf::detail::count(ssne::perf::detail::Vertices,count);
    const auto inputs=plan.inputs;const auto* ram=ssne::g_ram;
    auto read=[&](const PackedVertexPlan::Attribute& d,const uint8_t* p){Reader r;
        if(d.type==1)r.p=p+d.offset;
        else {const auto index=d.type==2?p[d.offset]:(uint32_t(p[d.offset])<<8|p[d.offset+1]);r.p=ram+plan.base[d.array]+index*plan.step[d.array];}
        return r;};
    for(unsigned v=0;v<count;++v,data+=plan.input_stride,dest+=plan.stride){
        const auto matrix=plan.position_matrix_stream?data[0]&63:plan.position_matrix;
        const auto& pos_d=plan.attributes[0];const auto& nrm_d=plan.attributes[1];
        float pos[4]={0,0,0,1};if(pos_d.type)read(pos_d,data).components(pos_d.fmt,pos_d.n,pos_d.scale,pos);
        std::memcpy(dest,pos,16);std::memcpy(dest+36+12*inputs,pos,12);const float position_index=float(matrix);std::memcpy(dest+48+12*inputs,&position_index,4);
        float normal[3]={0,0,1};if(nrm_d.type)read(nrm_d,data).components(nrm_d.fmt,3,nrm_d.scale,normal);
        std::memcpy(dest+24+12*inputs,normal,12);
        for(unsigned k=0;k<2;++k){const auto& d=plan.attributes[2+k];if(d.type)read(d,data).color_bytes(d.fmt,dest+16+k*4);else std::memset(dest+16+k*4,255,4);}
        for(unsigned k=0;k<inputs;++k){const auto& d=plan.attributes[4+k];float tc[3]={0,0,0};
            if(plan.tex_matrix_offset[k]>=0)tc[2]=float(data[plan.tex_matrix_offset[k]]&63);else std::memcpy(tc+2,plan.tex_matrix+k,4);
            if(d.type)read(d,data).components(d.fmt,d.n,d.scale,tc);std::memcpy(dest+24+k*12,tc,12);
        }
    }
    return true;
}

bool decode_gpu_vertex_plan_into(const PackedVertexPlan& plan,const uint8_t* data,uint32_t count,uint8_t* dest,size_t size){
    static const bool enabled=[] {const auto e=std::getenv("SSNE_GPU_TYPED_COMPONENTS");return !e||std::strcmp(e,"0");}();
    if(enabled&&plan.quantized_components)return decode_gpu_quantized_vertex_plan_into(plan,data,count,dest,size);
    return decode_gpu_float_vertex_plan_into(plan,data,count,dest,size);
}

template<class Bytes> static bool decode_gpu_packed_impl(int vat,const uint8_t* data,uint32_t count,Bytes& out,uint32_t& stride){
    const uint32_t layout=gpu_packed_stride(count);
    if(!layout)return false;
    const unsigned inputs=(layout-52)/12;
    ssne::perf::detail::Scope perf_detail(ssne::perf::detail::VertexDecode);
    ssne::perf::detail::count(ssne::perf::detail::Vertices,count);
    const uint32_t lo=g.cp[0x50],hi=g.cp[0x60],a=vat_reg(vat,0);
    // The measured world/model format accounts for most input vertices:
    // indexed16 f32 position/normal/UV, indexed8/16 RGBA8, no per-vertex matrix.
    // Prove every possible index stays on the original direct-RAM path before
    // using this fixed layout. Unusual bindings retain the checked decoder.
    constexpr uint32_t common_mask=1u|(7u<<1)|(1u<<9)|(7u<<10)|(7u<<14)|(1u<<21)|(7u<<22);
    constexpr uint32_t common_value=1u|(4u<<1)|(4u<<10)|(5u<<14)|(1u<<21)|(4u<<22);
    if((lo==0x5E00u || lo==0x7E00u) && hi==3u && (a&common_mask)==common_value && inputs>=1 && inputs<=2){
        const bool color16=lo==0x7E00u;const unsigned input_stride=color16?8:7,uv_offset=color16?6:5;
        const unsigned arrays[4]={0,1,2,4};uint32_t base[4],step[4];bool direct=true;
        for(unsigned k=0;k<4;++k){base[k]=g.cp[0xA0+arrays[k]]&0x03FFFFFFu;step[k]=g.cp[0xB0+arrays[k]]&255u;
            direct&=uint64_t(base[k])+uint64_t(k==2&&!color16?255u:65535u)*step[k]<ssne::kRamSize-64u;}
        if(direct){
            stride=52+12*inputs;
            if constexpr(std::is_same_v<Bytes,VertexBytes>)out.resize_for_overwrite(size_t(count)*stride);else out.resize(size_t(count)*stride);
            const auto mia=g.xf_reg[0x18];const float matrix=float(mia&63u);const uint32_t one=0x3F800000u,white=0xFFFFFFFFu;
            uint32_t tex_matrix[2];for(unsigned k=0;k<2;++k){const float value=float((mia>>(6+6*k))&63u);std::memcpy(tex_matrix+k,&value,4);}
            const uint32_t second_uv[3]={0,0,tex_matrix[1]};const auto* p=data;auto* dest=out.data();const auto* ram=ssne::g_ram;
            for(unsigned v=0;v<count;++v,p+=input_stride,dest+=stride){
                const uint32_t indices[4]={uint32_t(p[0])<<8|p[1],uint32_t(p[2])<<8|p[3],color16?(uint32_t(p[4])<<8|p[5]):p[4],uint32_t(p[uv_offset])<<8|p[uv_offset+1]};
                uint32_t position[4],normal[3],uv[3];position[3]=one;uv[2]=tex_matrix[0];
                std::memcpy(position,ram+base[0]+indices[0]*step[0],12);std::memcpy(normal,ram+base[1]+indices[1]*step[1],12);std::memcpy(uv,ram+base[3]+indices[3]*step[3],8);
                for(unsigned k=0;k<3;++k){position[k]=SSNE_BSWAP32(position[k]);normal[k]=SSNE_BSWAP32(normal[k]);}
                uv[0]=SSNE_BSWAP32(uv[0]);uv[1]=SSNE_BSWAP32(uv[1]);
                std::memcpy(dest,position,16);std::memcpy(dest+16,ram+base[2]+indices[2]*step[2],4);std::memcpy(dest+20,&white,4);
                std::memcpy(dest+24,uv,12);if(inputs==2)std::memcpy(dest+36,second_uv,12);
                std::memcpy(dest+24+12*inputs,normal,12);std::memcpy(dest+36+12*inputs,position,12);std::memcpy(dest+48+12*inputs,&matrix,4);
            }
            return true;
        }
    }
    const auto vsize=vertex_size(vat);
    const uint32_t mia=g.xf_reg[0x18],mib=g.xf_reg[0x19],tex_mi_mask=(lo>>1)&255;
    uint32_t def_tex_idx[8];
    for(unsigned t=0;t<8;++t)def_tex_idx[t]=t<4?(mia>>(6+6*t))&63:(mib>>(6*(t-4)))&63;
    AttrDesc pos_d, nrm_d, col_d[2], tex_d[8];
    pos_d.type = (lo >> 9) & 3;
    if (pos_d.type) {
        pos_d.fmt = (a >> 1) & 7; pos_d.n = (a & 1) ? 3 : 2;
        pos_d.scale = pos_d.fmt == 4 ? 1.f : 1.f / float(1u << ((a >> 4) & 31));
        pos_d.direct = pos_d.n * uint32_t(comp_size(pos_d.fmt)); pos_d.array = 0;
    }
    nrm_d.type = (lo >> 11) & 3;
    bool nbt = (a >> 9) & 1, idx3 = (a >> 31) & 1;
    uint32_t nrm_skip = 0;
    if (nrm_d.type) {
        static const float nscale[5] = {1.f / 128, 1.f / 64, 1.f / 32768, 1.f / 16384, 1.f};
        nrm_d.fmt = (a >> 10) & 7;
        nrm_d.scale = nscale[std::min<uint32_t>(nrm_d.fmt, 4)];
        nrm_d.direct = (nbt ? 9 : 3) * uint32_t(comp_size(nrm_d.fmt)); nrm_d.array = 1;
        if (nrm_d.type != 1 && nbt && idx3) nrm_skip = 2 * uint32_t(index_size(nrm_d.type));
    }
    for (int c = 0; c < 2; c++) {
        col_d[c].type = (lo >> (13 + 2 * c)) & 3;
        col_d[c].fmt = c == 0 ? (a >> 14) & 7 : (a >> 18) & 7;
        col_d[c].direct = uint32_t(color_size(col_d[c].fmt)); col_d[c].array = 2 + c;
    }
    for (int t = 0; t < 8; t++) {
        AttrDesc& d = tex_d[t];
        d.type = (hi >> (2 * t)) & 3;
        if (!d.type) continue;
        uint32_t reg = vat_reg(vat, kTexVat[t].reg);
        d.fmt = (reg >> kTexVat[t].fmt) & 7; d.n = ((reg >> kTexVat[t].cnt) & 1) ? 2 : 1;
        d.scale = d.fmt == 4 ? 1.f : 1.f / float(1u << tex_shift(vat, t));
        d.direct = d.n * uint32_t(comp_size(d.fmt)); d.array = 4 + t;
    }
        uint32_t offset = (lo & 1) ? 1 : 0;
        int matrix_offsets[8];
        for (unsigned t=0;t<8;++t) {
            matrix_offsets[t] = (tex_mi_mask & (1u<<t)) ? int(offset++) : -1;
        }
        auto stream_offset = [&](const AttrDesc& d) {
            const auto result=offset;
            if(d.type)offset+=d.type==1?d.direct:uint32_t(index_size(d.type));
            return result;
        };
        const auto pos_offset=stream_offset(pos_d), normal_offset=stream_offset(nrm_d);
        offset+=nrm_skip;
        uint32_t color_offsets[2],tex_offsets[8];
        for(unsigned c=0;c<2;++c)color_offsets[c]=stream_offset(col_d[c]);
        unsigned active_tex[8],active_count=0;
        for(unsigned t=0;t<8;++t){
            tex_offsets[t]=stream_offset(tex_d[t]);
            if(tex_d[t].type)active_tex[active_count++]=t;
        }

    // Match the existing GPU vertex layout directly. The shader, XF snapshots,
    // draw order and later camera interpolation all keep their original input.
    // CP array bindings cannot change inside one serialized FIFO primitive.
    // Read them once here; packed byte stores must not cause global CP/RAM
    // pointer reloads between the attributes of every vertex. No cross-draw
    // cache: the next primitive observes all current bases, strides and bytes.
    uint32_t bases[12],strides[12];const auto* ram=ssne::g_ram;
    for(unsigned i=0;i<12;++i){bases[i]=g.cp[0xA0+i]&0x03FFFFFF;strides[i]=g.cp[0xB0+i]&255;}
    auto read=[&](const AttrDesc& d,const uint8_t* p,uint32_t off){
        Reader r;
        if(d.type==1){r.p=p+off;return r;}
        const uint32_t index=d.type==2?p[off]:(uint32_t(p[off])<<8|p[off+1]);
        r.addr=bases[d.array]+index*strides[d.array];r.mem=true;
        if(r.addr<ssne::kRamSize-64 && (r.addr>>24)!=0xCC){r.p=ram+r.addr;r.mem=false;}
        return r;
    };
    stride=52+12*inputs;
    if constexpr(std::is_same_v<Bytes,VertexBytes>)out.resize_for_overwrite(size_t(count)*stride);else out.resize(size_t(count)*stride);
    const auto* p=data;auto* dest=out.data();
    for(unsigned v=0;v<count;++v,p+=vsize,dest+=stride){
        const unsigned matrix=(lo&1)?p[0]&63:mia&63;
        float pos[4]={0,0,0,1};
        if(pos_d.type){const auto r=read(pos_d,p,pos_offset);r.components(pos_d.fmt,pos_d.n,pos_d.scale,pos);}
        std::memcpy(dest,pos,16);std::memcpy(dest+36+12*inputs,pos,12);
        const float index=float(matrix);std::memcpy(dest+48+12*inputs,&index,4);
        float normal[3]={0,0,1};
        if(nrm_d.type){const auto r=read(nrm_d,p,normal_offset);r.components(nrm_d.fmt,3,nrm_d.scale,normal);}
        std::memcpy(dest+24+12*inputs,normal,12);
        for(unsigned c=0;c<2;++c){auto* color=dest+16+c*4;
            if(col_d[c].type)read(col_d[c],p,color_offsets[c]).color_bytes(col_d[c].fmt,color);
            else std::memset(color,255,4);
        }
        for(unsigned t=0;t<inputs;++t){
            float tc[3]={0,0,float(matrix_offsets[t]>=0?p[matrix_offsets[t]]&63:def_tex_idx[t])};
            const auto& d=tex_d[t];if(d.type){const auto r=read(d,p,tex_offsets[t]);r.components(d.fmt,d.n,d.scale,tc);}
            std::memcpy(dest+24+t*12,tc,12);
        }
    }
    return true;
}

// Bounded writer over bytes reserved by the owning frame. The unchanged
// decoder writes its complete payload; resize never allocates or zeroes it.
struct GpuPackedDestination {
    uint8_t* bytes;size_t capacity;
    uint8_t* data(){return bytes;}
    void resize(size_t count){if(count!=capacity)ssne::fatal("Direct GPU vertex size mismatch");}
};
bool decode_gpu_packed_into(int vat,const uint8_t* stream,uint32_t count,uint8_t* destination,size_t size,uint32_t& stride){
    GpuPackedDestination out{destination,size};
    return decode_gpu_packed_impl(vat,stream,count,out,stride);
}

void decode_vertices_fast(int vat,const uint8_t* data,uint32_t count,std::vector<Vtx>& out){
    decode_vertices_impl(vat,data,count,out,false);
}
bool decode_gpu_packed(int vat,const uint8_t* data,uint32_t count,std::vector<uint8_t>& out,uint32_t& stride){return decode_gpu_packed_impl(vat,data,count,out,stride);}
bool decode_gpu_packed(int vat,const uint8_t* data,uint32_t count,VertexBytes& out,uint32_t& stride){return decode_gpu_packed_impl(vat,data,count,out,stride);}

void decode_vertices(int vat, const uint8_t* data, uint32_t count, std::vector<Vtx>& out, bool allow_gpu) {
    ssne::perf::detail::Scope perf_detail(ssne::perf::detail::VertexDecode);
    ssne::perf::detail::count(ssne::perf::detail::Vertices,count);
    static const bool check = getenv("SSNE_CHECK_VERTICES") != nullptr;
    static const bool cpu_xf = getenv("SSNE_CPU_XF") != nullptr;
    decode_vertices_impl(vat,data,count,out,allow_gpu && !check && !cpu_xf);
    if (check) {  // debugging: compare with the reference decoder bit for bit
        static std::vector<Vtx> ref;
        static uint64_t checked = 0, bad = 0;
        decode_vertices_ref(vat, data, count, ref);
        for (uint32_t i = 0; i < count; i++)
            if (memcmp(&ref[i], &out[i], offsetof(Vtx, sx)) || memcmp(ref[i].color, out[i].color, sizeof(out[i].color)) ||
                memcmp(ref[i].tex, out[i].tex, sizeof(out[i].tex))) {
                if (bad++ < 10) fprintf(stderr, "[gx] vertex decoder mismatch (vat %d, vertex %u of %u)\n", vat, i, count);
            }
        if (((checked += count) & 0xFFFFF) < count)
            fprintf(stderr, "[gx] vertex decoder check: %llu vertices, %llu mismatches\n", (unsigned long long)checked,
                    (unsigned long long)bad);
    }
}

} // namespace ssne::gx

