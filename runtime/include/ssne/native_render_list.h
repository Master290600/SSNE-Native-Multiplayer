#pragma once

// EU 0x80139688. Native traversal of the queued model render lists. Matrix
// arithmetic and GX submission retain the existing SDK boundaries and callers.
// The caller supplies the original 0xF0-byte guest ABI frame.
namespace ssne::native_render_list {
template<class Call, class Poll>
void draw(PPCContext& c, uint32_t sp, Call&& call, Poll&& poll) {
    const auto copy_matrix = [&](uint32_t dst, uint32_t src) {
        // Preserve the original two forward 24-byte copies, including overlap.
        for (unsigned block=0; block<2; ++block) {
            for (unsigned word=0; word<6; ++word)
                mem_write32(dst+block*24+word*4, mem_read32(src+block*24+word*4));
            if (!block) poll();
        }
    };
    const uint32_t matrices=(mem_read32(c.r[13]-0x6C0C)+31)&~31u;
    copy_matrix(sp+8,0x80292154);
    uint32_t group=mem_read32(c.r[13]-0x6C08);
    while (group) {
        call.template operator()<0x8011A4E0>(0x8013970C,{mem_read32(group+12)});
        call.template operator()<0x8011A6E4>(0x80139714,{mem_read32(group+16)});
        const auto array=[&](uint32_t descriptor,uint32_t attribute,uint32_t caller) {
            if (descriptor) call.template operator()<0x801D58D0>(caller,
                {attribute,mem_read32(descriptor),mem_read8(descriptor+15)});
        };
        array(mem_read32(mem_read32(group+8)+16),9,0x80139734);
        array(mem_read32(mem_read32(group+8)+20),10,0x80139754);
        array(mem_read32(mem_read32(group+8)+28),13,0x80139774);
        uint32_t instance=mem_read32(group+20),matrix=matrices;
        while (instance) {
            const auto transform=mem_read32(instance+4);
            copy_matrix(sp+0x68,sp+8);
            call.template operator()<0x801B9EAC>(0x801397EC,{sp+0x68,transform,sp+0x98});
            copy_matrix(sp+0x38,sp+0x98);
            call.template operator()<0x801B9E78>(0x80139844,{sp+0x38,matrix});
            matrix+=48;
            instance=mem_read32(instance);
            if (instance) poll();
        }
        uint32_t mesh=mem_read32(mem_read32(group+8)+4);
        while (mesh) {
            const auto material=mem_read32(mesh+0x48);
            if (!(mem_read16(material+8)&2)) {
                call.template operator()<0x8012680C>(0x80139880,{material,mem_read32(group+4)});
                const bool animated=mem_read32(group+16)!=0;
                instance=mem_read32(group+20);matrix=matrices;
                while (instance) {
                    if (animated) call.template operator()<0x8011A928>(0x801398A8,{mem_read32(instance+4)});
                    array(mem_read32(instance+8),11,animated?0x801398C4:0x80139978);
                    call.template operator()<0x801DAE0C>(animated?0x801398D0:0x80139984,{matrix,0});
                    call.template operator()<0x801DAE5C>(animated?0x801398DC:0x80139990,{matrix,0});
                    const auto list=mem_read32(mesh+12);
                    const auto mode=mem_read32(list+12);
                    if (mem_read32(c.r[13]-0x6B84)!=mode)
                        call.template operator()<0x80147F84>(animated?0x801398F4:0x801399A8,{mode});
                    if (mem_read32(list)&31) {
                        cr_set_bit(c,6,0);
                        call.template operator()<0x801B60D0>(animated?0x80139910:0x801399C4,{0x8021E348});
                    }
                    if (mem_read32(list+4)&31) {
                        cr_set_bit(c,6,0);
                        call.template operator()<0x801B60D0>(animated?0x8013992C:0x801399E0,{0x8021E36C});
                    }
                    const auto size=mem_read32(list+4),data=mem_read32(list);
                    matrix+=48;
                    call.template operator()<0x801DAA54>(animated?0x8013993C:0x801399F0,{data,size});
                    instance=mem_read32(instance);
                    if (instance) poll();
                }
            }
            mesh=mem_read32(mesh);
            if (mesh) poll();
        }
        group=mem_read32(group);
        if (group) poll();
    }
    call.template operator()<0x801DAEAC>(0x80139A1C,{0});
}
}
