// Original independently registered NPC parts: Legionary wheels, def38
// attachments and the boss fragments produced by80179F5C. Native geometry
// is recreated from a local asset key; motion/death stays host authoritative.
bool validate_native_npc_piece(const EntityState& e,std::string& error){
 auto fail=[&](const char*s){error=s;return false;};
 if(e.kind!=EntityKind::Physics || e.definition!=5 || !e.id || !validate_render_metadata(e,error))return fail("Invalid NPC part identity/render pose");
 for(const auto&[key,value]:e.properties){
  if(render_field(key))continue;
  if(key=="position" || key=="spawn_position"){
   auto p=std::get_if<std::array<float,3>>(&value);if(!p)return fail("Invalid NPC part position type");for(auto f:*p)if(!std::isfinite(f) || std::abs(f)>1000000)return fail("Invalid NPC part position");
  }else if(key=="part_model"){
   auto p=std::get_if<std::string>(&value);if(!p || p->size()>191 || !p->starts_with("Models/") || !p->ends_with(".ssm") || p->find("..")!=std::string::npos || p->find(':')!=std::string::npos || p->find('\\')!=std::string::npos)return fail("Invalid original NPC part asset key");
   for(auto ch:*p)if(ch<32 || ch>126)return fail("Invalid NPC part asset character");
  }else if(key=="part_side" || key=="part_mask" || key=="part_flags"){
   auto p=std::get_if<int64_t>(&value);if(!p || *p<0 || uint64_t(*p)>(key=="part_side"?1u:UINT32_MAX))return fail("Invalid NPC part flags");
  }else return fail("Unsupported NPC part field");
 }
 for(auto key:{"position","spawn_position","part_model","part_side","part_mask","part_flags","render_visible","render_row0","render_row1","render_row2"})if(!e.properties.contains(key))return fail("Incomplete NPC part state");
 return true;
}
uint32_t create_native_npc_piece(PPCContext& context,const EntityState& e,std::string& error){
 if(!context_ok(context) || !validate_native_npc_piece(e,error))return 0;
 for(auto a:{0x8011967Cu,0x80087E48u,0x8008764Cu,0x80116084u,0x801505F8u})if(!ssne_find_func(a)){error="Original NPC part factory unavailable";return 0;}
 StackScratch scratch(context);auto call=context;call.msr&=~0x8000u;call.r[1]-=0x400;mem_write32(call.r[1],context.r[1]);
 const auto& name=std::get<std::string>(e.properties.at("part_model"));auto resource=ssne::native_static_model_resource(name);
 if(!resource){
  const auto key=call.r[1]+0x100,out=call.r[1]+0xE0,folder=call.r[1]+0x200,manager=mem_read32(0x802ABAA0u);
  if(!readable(manager,0x6C)){error="NPC asset manager unavailable";return 0;}
  const auto split=name.find_last_of('/');const auto file=name.substr(split+1,name.size()-split-5),directory=name.substr(0,split);
  for(unsigned i=0;i<=file.size();++i)mem_write8(key+i,i<file.size()?file[i]:0);for(unsigned i=0;i<=directory.size();++i)mem_write8(folder+i,i<directory.size()?directory[i]:0);mem_write32(out,0);
  const auto previous=mem_read32(manager+0x68);mem_write32(manager+0x68,folder);
  call.r[3]=manager;call.r[4]=out;call.r[5]=key;ssne_call(call,0x8011967C);mem_write32(manager+0x68,previous);resource=mem_read32(out);
 }
 if(!readable(resource,0x34) || mem_read32(resource)!=0x8021DC50u){error="Original NPC part asset is unavailable";return 0;}
 call.r[3]=mem_read32(context.r[13]-0x6DE0);call.r[4]=0x130;ssne_call(call,0x80116084);const auto object=call.r[3];
 if(!readable(object,0x130)){error="NPC part allocation failed";return 0;}
 mem_write32(context.r[13]-0x6DF0,mem_read32(context.r[13]-0x6DF0)-mem_read32(object-0x18));
 call.r[3]=object;call.r[4]=unsigned(std::get<int64_t>(e.properties.at("part_side")));ssne_call(call,0x80087E48);
 // Registration invokes87288, which clears+CC. Match the original NPC
 // factory order: register first, then acquire the local resource proxy.
 call.r[3]=mem_read32(0x802ABAACu);call.r[4]=object;ssne_call(call,0x801505F8);
 call.r[3]=object;call.r[4]=resource;ssne_call(call,0x8008764C);
 // E0 suppresses the original pre-render physics integration (8802C).
 // This independent replica proxy receives its complete matrix from the host.
 mem_write32(object+0xC8,0);mem_write32(object+0xE0,1);
 return object;
}
