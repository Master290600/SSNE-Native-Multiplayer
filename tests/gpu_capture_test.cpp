#include "../runtime/src/gx/gx_capture_interpolation.h"
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <cstdio>
using namespace ssne::gx;
template<class T> void put(std::ofstream& f,const T& x){f.write(reinterpret_cast<const char*>(&x),sizeof(x));}
int main(int argc,char** argv) {
 assert(argc==2); std::string dir=argv[1];std::filesystem::create_directories(dir);
 std::string env=dir+":0:3";
#ifdef _WIN32
 _putenv_s("SSNE_GPU_CAPTURE",env.c_str());
#else
 setenv("SSNE_GPU_CAPTURE",env.c_str(),1);
#endif
 GpuFrame frame;frame.wide=4.f/3.f;frame.local_camera=true;frame.live_copies={7,8};
 frame.seq=101;frame.produced_at=10.;frame.presentation_generation=6;frame.camera_valid=true;
 frame.camera={1,0,0,0,0,1,0,0,0,0,1,0};frame.look.camera=frame.camera;
 frame.look.enabled=true;frame.look.epoch=12;frame.look.ack=30;frame.look.generation=4;
 frame.look.slot=2;frame.look.pitch_low=-1.2f;frame.look.pitch_high=.9f;frame.look.angles={.1f,.2f,.3f};
 auto tex=std::make_shared<Texture>();tex->w=640;tex->h=448;tex->id=9;tex->efb_copy=7;tex->copy_fmt=6;tex->tex_fmt=6;
 tex->copy_regions={{7,0,0,224},{8,0,224,224}};
 frame.textures.push_back(tex);frame.states.push_back(GpuState{});frame.states[0].cfg.tex[0]=tex.get();frame.states[0].perspective=true;frame.states[0].local_ui=true;frame.states[0].cull=2;
 GpuCmd cmd{};cmd.type=GpuCmd::COPY_TEX;cmd.state=0x81080000;cmd.copy_id=7;cmd.copy_fmt=6;cmd.copy_fmt_known=true;cmd.intensity=true;frame.cmds.push_back(cmd);
 GpuState world{};world.perspective=world.world_camera=true;world.render_proxy=42;world.cfg.tex[0]=tex.get();world.projection[0]=world.projection[2]=1;
 frame.states.push_back(world);GpuCmd draw{};draw.type=GpuCmd::DRAW;draw.state=1;draw.stride=36;draw.ntex=1;draw.vcount=3;draw.idx_count=3;
 frame.cmds.push_back(draw);frame.idx={0,1,2};frame.vtx.resize(108);
 for(unsigned i=0;i<3;++i){float point[4]={float(i)*.1f,.2f,-1,2};std::memcpy(frame.vtx.data()+i*36,point,16);}
 gpu_capture_frame(frame);std::string err;
 ++frame.seq;frame.produced_at+=1./60;frame.camera[3]=.001f;
 gpu_capture_frame(frame);
 ++frame.seq;frame.produced_at+=1./60;
 auto distinct=std::make_shared<Texture>(*tex);frame.textures[0]=distinct;
 frame.states[0].cfg.tex[0]=frame.states[1].cfg.tex[0]=distinct.get();gpu_capture_frame(frame);
 auto loaded=gpu_load_captured_frame(dir+"/frame_00000.bin",err);
 assert(loaded && loaded->local_camera && loaded->states[0].perspective && loaded->states[0].cull==2 && loaded->states[0].local_ui && loaded->live_copies==frame.live_copies);
 assert(loaded->textures[0]->copy_regions.size()==2 && loaded->textures[0]->copy_regions[1].dest_y==224);
 assert(loaded->cmds[0].state==0x81080000);
 assert(loaded->cmds[0].copy_fmt_known && loaded->cmds[0].intensity && loaded->cmds[0].copy_fmt==6);
 assert(loaded->capture_version==8 && loaded->seq==101 && loaded->produced_at==10. && loaded->presentation_generation==6 && loaded->camera_valid);
 assert(loaded->camera[3]==0 && loaded->look.camera==frame.look.camera && loaded->look.angles==frame.look.angles);
 assert(loaded->look.epoch==12 && loaded->look.ack==30 && loaded->look.generation==4 && loaded->look.slot==2);
 assert(loaded->look.enabled && loaded->look.pitch_low==-1.2f && loaded->look.pitch_high==.9f);
 assert(loaded->vtx==frame.vtx && loaded->idx==frame.idx && loaded->cmds.size()==frame.cmds.size());
 assert(loaded->cmds[1].vcount==3 && loaded->cmds[1].stride==36 && loaded->cmds[1].ntex==1);
 auto second=gpu_load_captured_frame(dir+"/frame_00001.bin",err),third=gpu_load_captured_frame(dir+"/frame_00002.bin",err);
 assert(second && third && loaded->capture_stream==second->capture_stream);
 assert(loaded->textures[0]!=second->textures[0]);
 assert(restore_capture_texture_identity(*loaded,*second,err));
 assert(loaded->textures[0]==second->textures[0] && second->states[1].cfg.tex[0]==loaded->textures[0].get());
 assert(third->capture_texture_ids!=second->capture_texture_ids);
 assert(restore_capture_texture_identity(*second,*third,err));assert(third->textures[0]!=second->textures[0]);
 auto bad=gpu_load_captured_frame(dir+"/frame_00001.bin",err);bad->capture_stream[0]^=1;
 assert(!restore_capture_texture_identity(*loaded,*bad,err));
 bad=gpu_load_captured_frame(dir+"/frame_00001.bin",err);
 auto changed=std::make_shared<Texture>(*bad->textures[0]);changed->rgba={7};
 bad->textures[0]=changed;bad->states[0].cfg.tex[0]=bad->states[1].cfg.tex[0]=changed.get();
 assert(!restore_capture_texture_identity(*loaded,*bad,err));assert(bad->textures[0]==changed);
 bad->cmds[1].vcount=UINT32_MAX;assert(!validate_interpolation_capture(*bad,err));
 std::ifstream original(dir+"/frame_00000.bin",std::ios::binary);std::vector<char> bytes((std::istreambuf_iterator<char>(original)),{});
 for(size_t size=0;size<bytes.size();size+=7){
  {std::ofstream f(dir+"/truncated.bin",std::ios::binary);f.write(bytes.data(),size);}
  assert(!gpu_load_captured_frame(dir+"/truncated.bin",err));
 }
 struct Legacy {DrawCfg cfg;float vp[6];uint32_t cull;};Legacy state{};state.cull=3;
 {std::ofstream f(dir+"/legacy.bin",std::ios::binary);f.write("SSNEGPUF",8);put(f,uint32_t(1));put(f,1.f);put(f,uint32_t(0));put(f,uint32_t(1));put(f,uint32_t(sizeof(state)));put(f,state);put(f,uint32_t(0));put(f,uint32_t(0));put(f,uint32_t(0));}
 loaded=gpu_load_captured_frame(dir+"/legacy.bin",err);assert(loaded && loaded->states[0].cull==3 && !loaded->states[0].perspective);
 assert(!validate_interpolation_capture(*loaded,err));
 {std::ofstream f(dir+"/v7.bin",std::ios::binary);f.write("SSNEGPUF",8);put(f,uint32_t(7));put(f,1.f);put(f,uint8_t(0));
  put(f,uint32_t(0));put(f,uint32_t(0));put(f,uint32_t(0));put(f,uint8_t(0));
  for(unsigned i=0;i<4;++i)put(f,uint32_t(0));}
 loaded=gpu_load_captured_frame(dir+"/v7.bin",err);assert(loaded && loaded->capture_version==7);
 assert(!validate_interpolation_capture(*loaded,err));
 puts("GPU capture: exact v8 camera/time/look, ownership identity, separate equal textures, mismatch/truncation rejection and legacy replay passed");
}
