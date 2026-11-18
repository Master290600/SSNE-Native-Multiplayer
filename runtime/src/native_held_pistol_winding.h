#pragma once
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace ssne::detail {
// A draw lease, not a model/resource cache. Each binding is checked against
// native ownership and the resource registry again at the material boundary.
struct HeldPistolBinding {
    uint32_t actor=0,proxy=0,resource=0,matrix=0,file=0,data=0;
    friend bool operator==(const HeldPistolBinding&,const HeldPistolBinding&)=default;
};
inline bool held_left_pistol_name(std::string_view name) {
    constexpr std::string_view key="mp_left_deserthawk.ssm";
    return name==key || (name.size()>key.size() && name.ends_with(key) &&
        name[name.size()-key.size()-1]=='/');
}
class HeldPistolWinding {
    struct Draw {
        const void* context=nullptr;
        uint32_t material_stack=0;
        HeldPistolBinding binding{};
    } draw_;
public:
    class Scope {
        HeldPistolWinding& owner_;
        Draw previous_;
    public:
        Scope(HeldPistolWinding& owner,const void* context,uint32_t stack,
              HeldPistolBinding binding={}) noexcept
            :owner_(owner),previous_(owner.draw_) {
            // Verified PAL 8013D4D0 frame80 + 8012ED78 frame118. Binding both
            // the context and this exact frame excludes interrupt/direct draws.
            owner.draw_={context,stack>=0x198u?stack-0x198u:0u,binding};
        }
        Scope(const Scope&)=delete;
        Scope& operator=(const Scope&)=delete;
        ~Scope(){owner_.draw_=previous_;}
    };
    std::optional<HeldPistolBinding> binding(const void* context,uint32_t stack,uint32_t caller) const noexcept {
        if(!draw_.binding.proxy || draw_.context!=context || !draw_.material_stack ||
           draw_.material_stack!=stack ||
           (caller!=0x8012F098u && caller!=0x8012F18Cu && caller!=0x8012F234u))return {};
        return draw_.binding;
    }
    template<class Context,class Original>
    bool material(Context& context,bool binding_live,uint16_t flags,Original&& original) const {
        const auto selected=binding(&context,context.r[1],context.lr);
        const auto incoming=context.r[4];
        // Limit the repair to the observed original back-cull case. Keep
        // two-sided, explicitly front-culled, or already flipped overrides.
        if(!selected || !binding_live || (flags&(1u|8u)) || incoming){
            std::forward<Original>(original)(context);return false;
        }
        // Original 8012680C uses this boolean only to choose front/back cull,
        // before its cull cache. Flags&1 (two-sided) still selects cull-none.
        context.r[4]=1u;
        try {std::forward<Original>(original)(context);}
        catch(...){context.r[4]=incoming;throw;}
        // On success retain every original return register, including r4.
        return true;
    }
};
}
