// Control flow carried from the five pinned EU decomp.wtf sources.
void mode_body(PPCContext& ctx){
    Frame frame(ctx,0x10,31);const int32_t arg0=int32_t(ctx.r[3]);
    if (int32_t(mem_read32(ctx.r[13]-0x6B88)) == arg0) {
        ctx.r[3]=uint32_t(-1);return;
    }
    mem_write32(ctx.r[13]-0x6B88,arg0);
    switch (arg0) {                                 /* irregular */
    case 0:
        invoke<0x801D75E8>(ctx,0x80148104,{1});
        invoke<0x801D4E38>(ctx,0x80148108,{});
        invoke<0x801D43B4>(ctx,0x80148114,{0, 1});
        invoke<0x801D43B4>(ctx,0x80148120,{9, 3});
        invoke<0x801D43B4>(ctx,0x8014812C,{0xA, 3});
        invoke<0x801D43B4>(ctx,0x80148138,{0xB, 2});
        invoke<0x801D43B4>(ctx,0x80148144,{0xD, 3});
        invoke<0x801D4E70>(ctx,0x8014815C,{0, 9, 1, 4, 0});
        invoke<0x801D4E70>(ctx,0x80148174,{0, 0xA, 0, 4, 0});
        invoke<0x801D4E70>(ctx,0x8014818C,{0, 0xB, 1, 5, 0});
        invoke<0x801D4E70>(ctx,0x801481A4,{0, 0xD, 1, 4, 0});
        mem_write32(ctx.r[13]-0x6B84,arg0);
        break;
    case 1:
        invoke<0x801D75E8>(ctx,0x801481B4,{1});
        invoke<0x801D4E38>(ctx,0x801481B8,{});
        invoke<0x801D43B4>(ctx,0x801481C4,{9, 3});
        invoke<0x801D43B4>(ctx,0x801481D0,{0xA, 3});
        invoke<0x801D43B4>(ctx,0x801481DC,{0xB, 2});
        invoke<0x801D43B4>(ctx,0x801481E8,{0xD, 3});
        invoke<0x801D4E70>(ctx,0x80148200,{0, 9, 1, 4, 0});
        invoke<0x801D4E70>(ctx,0x80148218,{0, 0xA, 0, 4, 0});
        invoke<0x801D4E70>(ctx,0x80148230,{0, 0xB, 1, 5, 0});
        invoke<0x801D4E70>(ctx,0x80148248,{0, 0xD, 1, 4, 0});
        mem_write32(ctx.r[13]-0x6B84,5);
        break;
    case 3:
        invoke<0x801D75E8>(ctx,0x8014825C,{2});
        invoke<0x801D4E38>(ctx,0x80148260,{});
        invoke<0x801D43B4>(ctx,0x8014826C,{9, 3});
        invoke<0x801D43B4>(ctx,0x80148278,{0xA, 3});
        invoke<0x801D43B4>(ctx,0x80148284,{0xB, 2});
        invoke<0x801D43B4>(ctx,0x80148290,{0xC, 2});
        invoke<0x801D43B4>(ctx,0x8014829C,{0xD, 3});
        invoke<0x801D43B4>(ctx,0x801482A8,{0xE, 3});
        invoke<0x801D43B4>(ctx,0x801482B4,{0xF, 3});
        invoke<0x801D43B4>(ctx,0x801482C0,{0x10, 3});
        invoke<0x801D4E70>(ctx,0x801482D8,{0, 9, 1, 4, 0});
        invoke<0x801D4E70>(ctx,0x801482F0,{0, 0xA, 0, 4, 0});
        invoke<0x801D4E70>(ctx,0x80148308,{0, 0xB, 1, 5, 0});
        invoke<0x801D4E70>(ctx,0x80148320,{0, 0xC, 1, 5, 0});
        invoke<0x801D4E70>(ctx,0x80148338,{0, 0xD, 1, 4, 0});
        invoke<0x801D4E70>(ctx,0x80148350,{0, 0xE, 1, 4, 0});
        invoke<0x801D4E70>(ctx,0x80148368,{0, 0xF, 1, 4, 0});
        invoke<0x801D4E70>(ctx,0x80148380,{0, 0x10, 1, 2, 0x10});
        mem_write32(ctx.r[13]-0x6B84,1);
        break;
    case 4:
        invoke<0x801D75E8>(ctx,0x80148394,{2});
        invoke<0x801D4E38>(ctx,0x80148398,{});
        invoke<0x801D43B4>(ctx,0x801483A4,{9, 3});
        invoke<0x801D43B4>(ctx,0x801483B0,{0xA, 3});
        invoke<0x801D43B4>(ctx,0x801483BC,{0xB, 2});
        invoke<0x801D43B4>(ctx,0x801483C8,{0xC, 2});
        invoke<0x801D43B4>(ctx,0x801483D4,{0xD, 3});
        invoke<0x801D43B4>(ctx,0x801483E0,{0xE, 3});
        invoke<0x801D43B4>(ctx,0x801483EC,{0xF, 3});
        invoke<0x801D43B4>(ctx,0x801483F8,{0x10, 3});
        invoke<0x801D43B4>(ctx,0x80148404,{0x11, 3});
        invoke<0x801D43B4>(ctx,0x80148410,{0x12, 3});
        invoke<0x801D43B4>(ctx,0x8014841C,{0x13, 3});
        invoke<0x801D43B4>(ctx,0x80148428,{0x14, 3});
        invoke<0x801D4E70>(ctx,0x80148440,{0, 9, 1, 4, 0});
        invoke<0x801D4E70>(ctx,0x80148458,{0, 0xA, 0, 4, 0});
        invoke<0x801D4E70>(ctx,0x80148470,{0, 0xB, 1, 5, 0});
        invoke<0x801D4E70>(ctx,0x80148488,{0, 0xC, 1, 5, 0});
        invoke<0x801D4E70>(ctx,0x801484A0,{0, 0xD, 1, 4, 0});
        invoke<0x801D4E70>(ctx,0x801484B8,{0, 0xE, 1, 4, 0});
        invoke<0x801D4E70>(ctx,0x801484D0,{0, 0xF, 1, 4, 0});
        invoke<0x801D4E70>(ctx,0x801484E8,{0, 0x10, 1, 2, 0x10});
        invoke<0x801D4E70>(ctx,0x80148500,{0, 0x11, 1, 2, 0x10});
        invoke<0x801D4E70>(ctx,0x80148518,{0, 0x12, 1, 2, 0x10});
        invoke<0x801D4E70>(ctx,0x80148530,{0, 0x13, 1, 2, 0x10});
        invoke<0x801D4E70>(ctx,0x80148548,{0, 0x14, 1, 2, 0x10});
        mem_write32(ctx.r[13]-0x6B84,3);
        break;
    case 5:
        invoke<0x801D75E8>(ctx,0x8014855C,{1});
        invoke<0x801D4E38>(ctx,0x80148560,{});
        invoke<0x801D43B4>(ctx,0x8014856C,{9, 3});
        invoke<0x801D43B4>(ctx,0x80148578,{0xA, 3});
        invoke<0x801D43B4>(ctx,0x80148584,{0xB, 3});
        invoke<0x801D43B4>(ctx,0x80148590,{0xD, 3});
        invoke<0x801D4E70>(ctx,0x801485A8,{0, 9, 1, 4, 0});
        invoke<0x801D4E70>(ctx,0x801485C0,{0, 0xA, 0, 4, 0});
        invoke<0x801D4E70>(ctx,0x801485D8,{0, 0xB, 1, 5, 0});
        invoke<0x801D4E70>(ctx,0x801485F0,{0, 0xD, 1, 4, 0});
        mem_write32(ctx.r[13]-0x6B84,2);
        break;
    case 7:
        invoke<0x801D75E8>(ctx,0x80148604,{1});
        invoke<0x801D4E38>(ctx,0x80148608,{});
        invoke<0x801D43B4>(ctx,0x80148614,{9, 1});
        invoke<0x801D43B4>(ctx,0x80148620,{0xB, 1});
        invoke<0x801D4E70>(ctx,0x80148638,{0, 9, 1, 4, 0});
        invoke<0x801D4E70>(ctx,0x80148914,{0, 0xB, 1, 5, 0});
        mem_write32(ctx.r[13]-0x6B84,-1);
        break;
    case 8:
        invoke<0x801D75E8>(ctx,0x80148654,{1});
        invoke<0x801D4E38>(ctx,0x80148658,{});
        invoke<0x801D43B4>(ctx,0x80148664,{9, 1});
        invoke<0x801D43B4>(ctx,0x80148670,{0xB, 1});
        invoke<0x801D4E70>(ctx,0x80148688,{0, 9, 1, 4, 0});
        invoke<0x801D4E70>(ctx,0x80148914,{0, 0xB, 1, 5, 0});
        mem_write32(ctx.r[13]-0x6B84,-1);
        break;
    case 9:
        invoke<0x801D75E8>(ctx,0x801486A4,{1});
        invoke<0x801D4E38>(ctx,0x801486A8,{});
        invoke<0x801D43B4>(ctx,0x801486B4,{9, 1});
        invoke<0x801D43B4>(ctx,0x801486C0,{0xB, 1});
        invoke<0x801D43B4>(ctx,0x801486CC,{0xD, 1});
        invoke<0x801D43B4>(ctx,0x801486D8,{0xE, 1});
        invoke<0x801D4E70>(ctx,0x801486F0,{0, 9, 1, 4, 0});
        invoke<0x801D4E70>(ctx,0x80148708,{0, 0xB, 1, 5, 0});
        invoke<0x801D4E70>(ctx,0x80148720,{0, 0xD, 1, 4, 0});
        invoke<0x801D4E70>(ctx,0x80148914,{0, 0xE, 1, 4, 0});
        mem_write32(ctx.r[13]-0x6B84,-1);
        break;
    case 10:
        invoke<0x801D75E8>(ctx,0x80148734,{1});
        invoke<0x801D4E38>(ctx,0x80148738,{});
        invoke<0x801D43B4>(ctx,0x80148744,{9, 1});
        invoke<0x801D43B4>(ctx,0x80148750,{0xB, 1});
        invoke<0x801D43B4>(ctx,0x8014875C,{0xD, 3});
        invoke<0x801D43B4>(ctx,0x80148768,{0xE, 3});
        invoke<0x801D4E70>(ctx,0x80148780,{0, 9, 1, 4, 0});
        invoke<0x801D4E70>(ctx,0x80148798,{0, 0xB, 1, 5, 0});
        invoke<0x801D4E70>(ctx,0x801487B0,{0, 0xD, 1, 4, 0});
        invoke<0x801D4E70>(ctx,0x80148914,{0, 0xE, 1, 4, 0});
        mem_write32(ctx.r[13]-0x6B84,-1);
        break;
    case 11:
        invoke<0x801D75E8>(ctx,0x801487C4,{1});
        invoke<0x801D4E38>(ctx,0x801487C8,{});
        invoke<0x801D43B4>(ctx,0x801487D4,{9, 1});
        invoke<0x801D43B4>(ctx,0x801487E0,{0xB, 1});
        invoke<0x801D4E70>(ctx,0x801487F8,{0, 9, 1, 4, 0});
        invoke<0x801D4E70>(ctx,0x80148914,{0, 0xB, 1, 5, 0});
        mem_write32(ctx.r[13]-0x6B84,-1);
        break;
    case 13:
        invoke<0x801D75E8>(ctx,0x80148814,{1});
        invoke<0x801D4E38>(ctx,0x80148818,{});
        invoke<0x801D43B4>(ctx,0x80148824,{9, 1});
        invoke<0x801D43B4>(ctx,0x80148830,{0xB, 1});
        invoke<0x801D43B4>(ctx,0x8014883C,{0xD, 2});
        invoke<0x801D43B4>(ctx,0x80148848,{0xE, 2});
        invoke<0x801D4E70>(ctx,0x80148860,{0, 9, 1, 4, 0});
        invoke<0x801D4E70>(ctx,0x80148878,{0, 0xB, 1, 3, 0});
        invoke<0x801D4E70>(ctx,0x80148890,{0, 0xD, 1, 4, 0});
        invoke<0x801D4E70>(ctx,0x80148914,{0, 0xE, 1, 4, 0});
        mem_write32(ctx.r[13]-0x6B84,-1);
        break;
    case 12:
    case 14:
    case 6:
        invoke<0x801D75E8>(ctx,0x801488A4,{1});
        invoke<0x801D4E38>(ctx,0x801488A8,{});
        invoke<0x801D43B4>(ctx,0x801488B4,{9, 1});
        invoke<0x801D43B4>(ctx,0x801488C0,{0xB, 1});
        invoke<0x801D43B4>(ctx,0x801488CC,{0xD, 1});
        invoke<0x801D4E70>(ctx,0x801488E4,{0, 9, 1, 4, 0});
        invoke<0x801D4E70>(ctx,0x801488FC,{0, 0xB, 1, 5, 0});
        invoke<0x801D4E70>(ctx,0x80148914,{0, 0xD, 1, 4, 0});
        mem_write32(ctx.r[13]-0x6B84,-1);
        break;
    case 15:
        invoke<0x801D75E8>(ctx,0x801488A4,{1});
        invoke<0x801D4E38>(ctx,0x801488A8,{});
        invoke<0x801D43B4>(ctx,0x801488B4,{9, 1});
        invoke<0x801D43B4>(ctx,0x801488C0,{0xB, 1});
        invoke<0x801D43B4>(ctx,0x801488CC,{0xD, 1});
        invoke<0x801D4E70>(ctx,0x801488E4,{0, 9, 1, 4, 0});
        invoke<0x801D4E70>(ctx,0x801488FC,{0, 0xB, 1, 5, 0});
        invoke<0x801D4E70>(ctx,0x80148914,{0, 0xD, 1, 4, 0});
        mem_write32(ctx.r[13]-0x6B84,-1);
        break;
    case 16:
        invoke<0x801D75E8>(ctx,0x80148928,{1});
        invoke<0x801D4E38>(ctx,0x8014892C,{});
        invoke<0x801D43B4>(ctx,0x80148938,{9, 3});
        invoke<0x801D43B4>(ctx,0x80148944,{0xA, 3});
        invoke<0x801D43B4>(ctx,0x80148950,{0xB, 3});
        invoke<0x801D43B4>(ctx,0x8014895C,{0xD, 3});
        invoke<0x801D4E70>(ctx,0x80148974,{0, 9, 1, 4, 0});
        invoke<0x801D4E70>(ctx,0x8014898C,{0, 0xA, 0, 4, 0});
        invoke<0x801D4E70>(ctx,0x801489A4,{0, 0xB, 1, 5, 0});
        invoke<0x801D4E70>(ctx,0x801489BC,{0, 0xD, 1, 4, 0});
        mem_write32(ctx.r[13]-0x6B84,4);
        break;
    default:
        cr_set_bit(ctx,6,0);invoke<0x80004254>(ctx,0x801489D8,{0x8021EFA8});
        ctx.r[3]=uint32_t(1);return;
    }
    ctx.r[3]=uint32_t(0);return;
}
void material_body(PPCContext& ctx){
    Frame frame(ctx,0x10,31);const int32_t arg0=int32_t(ctx.r[3]);
    auto stage=[&](){return mem_read32(ctx.r[13]-0x5C84);};
    int32_t mode = 1000;
    mem_write32(0x802442B4+stage()*4,arg0);
    switch (arg0) {
    case -1: mode = -1; break;
    case 1000:
        invoke<0x801D8E78>(ctx,0x80123520,{uint32_t(stage()), 15, 15, 15, 10});
        break;
    case 1001:
        invoke<0x801D8E78>(ctx,0x801236DC,{uint32_t(stage()), 15, 15, 15, 8});
        break;
    case 1002:
        invoke<0x801D8E78>(ctx,0x80123554,{uint32_t(stage()), 15, 8, 12, 15});
        mode = 1001;
        break;
    case 1003:
        invoke<0x801D8E78>(ctx,0x801237D0,{uint32_t(stage()), 15, 15, 15, 0});
        break;
    case 2000:
        invoke<0x801D8E78>(ctx,0x801237EC,{uint32_t(stage()), 15, 8, 10, 15});
        break;
    case 2001:
        invoke<0x801D8E78>(ctx,0x80123738,{uint32_t(stage()), 15, 8, 10, 15});
        mode = 1003;
        break;
    case 2002:
        invoke<0x801D8E78>(ctx,0x801237EC,{uint32_t(stage()), 13, 8, 11, 15});
        break;
    case 2007:
        invoke<0x801D8E78>(ctx,0x801237EC,{uint32_t(stage()), 15, 8, 0, 15});
        break;
    case 2008:
        invoke<0x801D8E78>(ctx,0x80123738,{uint32_t(stage()), 15, 8, 0, 15});
        mode = 1003;
        break;
    case 2003:
        invoke<0x801D8E78>(ctx,0x801235E8,{uint32_t(stage()), 15, 0, 8, 9});
        break;
    case 2004:
        invoke<0x801D8E78>(ctx,0x801237D0,{uint32_t(stage()), 15, 8, 2, 0});
        break;
    case 2009:
        invoke<0x801D8E78>(ctx,0x801237D0,{uint32_t(stage()), 15, 8, 1, 0});
        break;
    case 2005:
        invoke<0x801D8E78>(ctx,0x80123738,{uint32_t(stage()), 15, 0, 4, 15});
        mode = 1003;
        break;
    case 2006:
        invoke<0x801D8E78>(ctx,0x80123698,{uint32_t(stage()), 15, 8, 0, 15});
        mode = 1002;
        break;
    case 4000:
        invoke<0x801D8E78>(ctx,0x801237D0,{uint32_t(stage()), 15, 12, 8, 0});
        break;
    case 4001: {
        const int32_t color = 15;
        invoke<0x801D8E78>(ctx,0x8012366C,{uint32_t(stage()), color, 14, 8, 0});
        invoke<0x801D912C>(ctx,0x80123678,{uint32_t(stage()), 6});
        break;
    }
    case 4002:
        invoke<0x801D8E78>(ctx,0x80123698,{uint32_t(stage()), 15, 12, 10, 8});
        mode = 1002;
        break;
    case 3000:
        invoke<0x801D8E78>(ctx,0x801237EC,{uint32_t(stage()), 8, 10, 11, 15});
        break;
    case 3001:
        invoke<0x801D8E78>(ctx,0x801237EC,{uint32_t(stage()), 0, 8, 9, 15});
        break;
    case 3100:
        invoke<0x801D8E78>(ctx,0x801236DC,{uint32_t(stage()), 0, 15, 9, 8});
        break;
    case 3002:
        invoke<0x801D8E78>(ctx,0x801237EC,{uint32_t(stage()), 0, 8, 1, 15});
        break;
    case 3003:
        invoke<0x801D8E78>(ctx,0x801237EC,{uint32_t(stage()), 0, 2, 3, 15});
        break;
    case 3008:
        invoke<0x801D8E78>(ctx,0x801237EC,{uint32_t(stage()), 0, 8, 3, 15});
        break;
    case 3004:
        invoke<0x801D8E78>(ctx,0x80123738,{uint32_t(stage()), 0, 4, 6, 15});
        mode = 1003;
        break;
    case 3006:
        invoke<0x801D8E78>(ctx,0x801237EC,{uint32_t(stage()), 0, 15, 9, 15});
        break;
    case 3005:
        invoke<0x801D8E78>(ctx,0x801237EC,{uint32_t(stage()), 0, 13, 9, 15});
        break;
    case 3007:
        invoke<0x801D8E78>(ctx,0x801237EC,{uint32_t(stage()), 0, 12, 9, 15});
        break;
    case 9000:
        invoke<0x801D8E78>(ctx,0x80123794,{uint32_t(stage()), 15, 8, 14, 2});
        mode = 1004;
        break;
    case 9001:
        invoke<0x801D8E78>(ctx,0x801237B4,{uint32_t(stage()), 15, 8, 14, 0});
        mode = 1005;
        break;
    case 9002:
        invoke<0x801D8E78>(ctx,0x801237D0,{uint32_t(stage()), 15, 8, 12, 0});
        break;
    case 9003:
        invoke<0x801D8E78>(ctx,0x801237EC,{uint32_t(stage()), 1, 0, 14, 15});
        break;
    case 5000:
        invoke<0x801D8E78>(ctx,0x8012380C,{uint32_t(stage()), 15, 15, 15, 0});
        mode = 1006;
        break;
    case 5001:
        invoke<0x801D8E78>(ctx,0x8012382C,{uint32_t(stage()), 15, 0, 12, 13});
        mode = 1007;
        break;
    }
    ctx.r[3]=stage();
    if (int32_t(mem_read32(0x802442F4+stage()*4)) != mode) {
        mem_write32(0x802442F4+stage()*4,mode);
        switch (mode) {
        case 1000: invoke<0x801D8F00>(ctx,0x80123924,{uint32_t(stage()), 0, 0, 0, 1, 0}); break;
        case 1001: invoke<0x801D8F00>(ctx,0x801238B4,{uint32_t(stage()), 0, 0, 0, 1, 1}); break;
        case 1002: invoke<0x801D8F00>(ctx,0x801238D0,{uint32_t(stage()), 0, 0, 0, 1, 2}); break;
        case 1003: invoke<0x801D8F00>(ctx,0x80123924,{uint32_t(stage()), 0, 0, 1, 1, 0}); break;
        case 1004: invoke<0x801D8F00>(ctx,0x80123924,{uint32_t(stage()), 0, 0, 0, 0, 0}); break;
        case 1005: invoke<0x801D8F00>(ctx,0x80123924,{uint32_t(stage()), 0, 0, 1, 0, 0}); break;
        case 1006: invoke<0x801D8F00>(ctx,0x80123924,{uint32_t(stage()), 0, 2, 0, 1, 0}); break;
        case 1007: invoke<0x801D8F00>(ctx,0x80123940,{uint32_t(stage()), 1, 0, 0, 1, 0}); break;
        }
    }
}
void install(){
    if(mem_read32(0x80000000)!=0x47334250u)return;
    auto fingerprint=[](uint32_t a,uint32_t n,uint64_t expected){uint64_t h=14695981039346656037ull;for(unsigned i=0;i<n;++i){h^=mem_read8(a+i);h*=1099511628211ull;}return h==expected;};
    if(!ssne_find_func(0x80148048) || !fingerprint(0x80148048,2480,0x3F01764900AA7CEEull))return;
    if(!ssne_find_func(0x801233B0) || !fingerprint(0x801233B0,1444,0xF81A12F7CD49A1D6ull))return;
    if(!ssne_find_func(0x801493D0) || !fingerprint(0x801493D0,548,0x93CB2393928A40FAull))return;
    if(!ssne_find_func(0x8014CC6C) || !fingerprint(0x8014CC6C,568,0x5295E35BB62F3210ull))return;
    if(!ssne_find_func(0x80129958) || !fingerprint(0x80129958,152,0xBA55084DAA7FB0E0ull))return;
    mode_original=ssne_find_func(0x80148048)->func;ssne_install_hook(0x80148048,mode);
    material_original=ssne_find_func(0x801233B0)->func;ssne_install_hook(0x801233B0,material);
    visibility_original=ssne_find_func(0x801493D0)->func;ssne_install_hook(0x801493D0,visibility);
    projection_original=ssne_find_func(0x8014CC6C)->func;ssne_install_hook(0x8014CC6C,projection);
    available=true;log_message("[ssne] five matched EU render/skinning paths adapted to native PC C++; native path timers sample one in16 calls");
}
