#include "../runtime/src/gx/gx_gpu_frame.h"
#include <cassert>
#include <cstdio>
using namespace ssne::gx;
GpuCmd command(GpuCmd::Type type, uint64_t id=0) { GpuCmd c{};c.type=type;c.copy_id=id;return c; }
int main() {
    // A grass render-to-texture frame disappears during a render hitch.
    auto grass=std::make_unique<GpuFrame>();
    grass->cmds={command(GpuCmd::CLEAR),command(GpuCmd::DRAW),command(GpuCmd::COPY_TEX,41),command(GpuCmd::DRAW),command(GpuCmd::COPY_XFB)};
    GpuFrame queued;
    preserve_texture_work(grass,queued);
    assert(!grass && queued.prerequisites.size()==1);
    assert(queued.prerequisites[0]->cmds.size()==3);
    assert(queued.prerequisites[0]->cmds.back().copy_id==41);
    // That queued frame is dropped too: grass creation still precedes water creation.
    auto water=std::make_unique<GpuFrame>(std::move(queued));
    water->cmds={command(GpuCmd::DRAW),command(GpuCmd::COPY_TEX,42),command(GpuCmd::COPY_XFB)};
    GpuFrame visible;
    preserve_texture_work(water,visible);
    assert(!water && visible.prerequisites.size()==2);
    assert(visible.prerequisites[0]->cmds.back().copy_id==41);
    assert(visible.prerequisites[1]->cmds.back().copy_id==42);
    // Ordinary pictures are discarded; inherited texture work survives.
    auto ordinary=std::make_unique<GpuFrame>(std::move(visible));
    ordinary->cmds={command(GpuCmd::DRAW),command(GpuCmd::COPY_XFB)};
    GpuFrame last;
    preserve_texture_work(ordinary,last);
    assert(ordinary && last.prerequisites.size()==2);
    auto disposable=std::make_unique<GpuFrame>();
    disposable->cmds={command(GpuCmd::DRAW)};
    GpuFrame empty;
    preserve_texture_work(disposable,empty);
    assert(disposable && empty.prerequisites.empty());
    // A retained water copy samples the earlier grass copy: preserve both,
    // even when only water is directly referenced by future frames.
    auto chain=std::make_unique<GpuFrame>();
    auto base=std::make_unique<GpuFrame>();
    base->cmds={command(GpuCmd::COPY_TEX,71)};
    chain->prerequisites.push_back(std::move(base));
    auto derived=std::make_unique<GpuFrame>();
    auto source=std::make_shared<Texture>();source->efb_copy=71;
    derived->textures.push_back(source);
    derived->states.push_back(GpuState{});
    derived->states[0].cfg.tex[0]=source.get();
    derived->cmds={command(GpuCmd::DRAW),command(GpuCmd::COPY_TEX,72)};
    chain->prerequisites.push_back(std::move(derived));
    prune_texture_work(*chain,{72});
    assert(chain->prerequisites.size()==2);
    // Superseded per-frame copies must not accumulate during a long hitch.
    auto pending=std::make_unique<GpuFrame>();
    for (uint64_t id=1; id<=1000; ++id) {
        pending->cmds={command(GpuCmd::COPY_TEX,id)};
        GpuFrame following;
        preserve_texture_work(pending,following);
        prune_texture_work(following,{id});
        assert(following.prerequisites.size()==1);
        pending=std::make_unique<GpuFrame>(std::move(following));
    }
    auto partial=std::make_unique<GpuFrame>();
    auto basecopy=std::make_unique<GpuFrame>();basecopy->cmds={command(GpuCmd::COPY_TEX,91)};
    auto patch=std::make_unique<GpuFrame>();patch->cmds={command(GpuCmd::COPY_TEX,92)};patch->cmds[0].base_copy_id=91;
    partial->prerequisites.push_back(std::move(basecopy));partial->prerequisites.push_back(std::move(patch));
    prune_texture_work(*partial,{92});assert(partial->prerequisites.size()==2);
    puts("GPU frame-drop texture dependency tests passed");
}
