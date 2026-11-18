#include "spectator.h"
#include "world_state.h"
#ifdef SSNE_TEST_SESSION_TRANSPORT
#include "authoritative_session.h"
#endif
#include "checkpoint_codec.h"
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace port::coop;
namespace {
unsigned checks=0;
void require(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
void authority_test(){
    SessionAuthority authority(3);const auto host=*authority.join("Host"),client=*authority.join("Client"),bot=*authority.join("Bot",true);for(const auto id:{host,client,bot})require(authority.attach_character(id,100+id.slot) && authority.finish_join(id),"Authority actor readiness failed");
    require(authority.request_spectator(client,1,true)==CommandResult::Applied,"Voluntary watching rejected");require(authority.participant(client)->spectator,"Watch preference missing");
    require(authority.request_spectator({client.slot,client.generation+1},2,false)==CommandResult::StalePlayer,"Forged generation changed spectator");
    authority.set_spectator_lock(1u<<client.slot);require(authority.request_spectator(client,2,false)==CommandResult::NotEligible,"Round lock escaped");authority.set_spectator_lock(0);
    require(authority.request_spectator(client,2,false)==CommandResult::StaleSequence,"Locked leave replay became valid after unlock");require(authority.request_spectator(client,3,false)==CommandResult::Applied,"Fresh leave after unlock rejected");
    auto scene=authority.begin_cutscene();authority.set_spectator_lock(1u<<client.slot);require(authority.skip_vote().required==1,"Forced spectator or AFK bot counted in skip electorate");require(authority.request_skip(host,1,scene)==CommandResult::Applied && authority.skip_vote().skip_requested,"Spectator blocked active human cutscene vote");require(authority.end_cutscene(scene),"Vote cleanup failed");
    require(authority.request_spectator(host,2,true)==CommandResult::Applied,"Host watcher rejected");const auto snapshot=authority.checkpoint_state();std::string error;require(authority.restore_checkpoint(snapshot,error),error.c_str());require(authority.participant(host)->spectator && authority.spectator_lock_mask()==(1u<<client.slot),"Exact rollback lost manual or forced spectator state");
    require(authority.leave(client),"Spectator leave failed");const auto replacement=*authority.join("Replacement");require(replacement.slot==client.slot && replacement.generation!=client.generation && !authority.participant(replacement)->spectator && !authority.spectator_lock_mask(),"Reused slot inherited old watcher lock");
    SessionAuthority denied(2);const auto denied_id=*denied.join("Carrier");require(denied.attach_character(denied_id,200) && denied.finish_join(denied_id),"Cancellation fixture join failed");
    require(denied.request_spectator(denied_id,100,true)==CommandResult::Applied && denied.cancel_spectator_activation(denied_id) && !denied.participant(denied_id)->spectator,"Native watch denial did not roll back voluntary state");
    require(denied.request_spectator(denied_id,100,true)==CommandResult::StaleSequence && denied.request_spectator(denied_id,101,true)==CommandResult::Applied,"Cancellation lowered replay floor or rejected fresh intent");
    require(!denied.cancel_spectator_activation({denied_id.slot,denied_id.generation+1}) && denied.participant(denied_id)->spectator,"Cancellation accepted stale generation");
    denied.set_spectator_lock(1u<<denied_id.slot);require(!denied.cancel_spectator_activation(denied_id) && denied.participant(denied_id)->spectator && denied.spectator_lock_mask(),"Native denial escaped forced round lock");
    denied.set_spectator_lock(0);require(denied.cancel_spectator_activation(denied_id) && denied.cancel_spectator_activation(denied_id),"Cancellation was not idempotent");
    // Pinned209 SAC1 layout: one connected ready Host, two-slot capacity.
    SessionAuthority old(2);const auto old_host=*old.join("Host");require(old.attach_character(old_host,100) && old.finish_join(old_host),"Legacy fixture setup");
    checkpoint::Writer legacy;legacy.u32(0x53414331);legacy.u32(2);legacy.u64(0);legacy.u64(0);legacy.flag(false);legacy.flag(false);
    for(unsigned slot=0;slot<max_players;++slot){const bool first=slot==0;legacy.u32(first?0:max_players);legacy.u64(first?1:0);legacy.flag(first);legacy.flag(first);legacy.flag(false);legacy.u64(first?100:0);legacy.text(first?"Host":"");legacy.u64(first?1:0);legacy.u64(0);legacy.flag(false);}
    require(old.restore_checkpoint(legacy.finish(),error) && !old.participant(old_host)->spectator && old.spectator_lock_mask()==0,"Legacy209 authority checkpoint migration failed");
}
void controller_test(){
    const PlayerId self{1,3},a{0,4},b{2,6},c{3,8};std::array<SpectatorTarget,4> targets{{{self,true,true,true,100},{a,true,true,false,100},{b,true,true,false,100},{c,true,true,true,100}}};SpectatorController controller;ssne::PadState pad;
    auto out=controller.update(1,self,targets,true,pad);require(out.target==a,"First valid watch target not selected");pad.buttons=ssne::PAD_RIGHT;out=controller.update(1,self,targets,true,pad);require(out.target==b,"Next target cycling failed");out=controller.update(1,self,targets,true,pad);require(out.target==b,"Held cycle repeated every frame");pad={};controller.update(1,self,targets,true,pad);pad.buttons=ssne::PAD_RIGHT;out=controller.update(1,self,targets,true,pad);require(out.target==a,"Watch cycle did not wrap");
    pad={};controller.update(1,self,targets,true,pad);pad.stick_x=0;out=controller.update(1,self,targets,true,pad);require(out.target==b,"Keyboard/controller stick cycle failed");
    pad.buttons=ssne::PAD_R|ssne::PAD_L|ssne::PAD_START|ssne::PAD_A;pad.trigger_l=pad.trigger_r=255;pad.cstick_x=255;out=controller.update(1,self,targets,true,pad);require(out.menu.buttons==ssne::PAD_START && !out.menu.trigger_l && !out.menu.trigger_r && out.menu.stick_x==128 && out.menu.cstick_x==128,"Watcher leaked movement/fire into gameplay");
    out=controller.update(1,self,targets,true,pad,true);require((out.menu.buttons&ssne::PAD_A) && out.menu.stick_x==0 && !out.menu.trigger_l && out.menu.cstick_x==128,"Watcher native menu input lost or unsafe");
    targets[2].health=0;out=controller.update(1,self,targets,true,{});require(out.target==a,"Dead watched player was retained");targets[1].player.generation++;out=controller.update(1,self,targets,true,{});require(out.target==targets[1].player,"New generation inherited stale watch identity");
    targets[1].health=NAN;out=controller.update(1,self,targets,true,{});require(!out.target,"Invalid/nonliving target admitted");targets[1].health=100;targets[3]=targets[1];out=controller.update(1,self,targets,true,{});require(!out.target,"Ambiguous duplicated slot admitted");
    out=controller.update(2,self,targets,true,pad);require(!out.target,"Epoch transition retained stale target");out=controller.update(2,self,targets,false,pad);require(!out.watching && out.menu.trigger_r==255 && !controller.target(),"Leaving watch changed ordinary human input");
}
void codec_test(){
    WorldState world;world.revision=1;world.level="DM01";world.players[0]={{0,1},true,false,false,0,"Host",true};std::string error;require(validate_world(world,error),error.c_str());
    auto bytes=encode_delta(difference(nullptr,world));WorldDelta decoded;require(decode_delta(bytes,decoded,error) && decoded.players[0].spectator,"Spectator full snapshot lost state");
    auto truncated=bytes;truncated.pop_back();require(!decode_delta(truncated,decoded,error),"Truncated modern spectator extension accepted");
    auto legacy=truncated;legacy[32]&=1;require(decode_delta(legacy,decoded,error) && !decoded.players[0].spectator,"Old209 world checkpoint could not migrate");
    bytes.push_back(0);require(!decode_delta(bytes,decoded,error),"Trailing malformed spectator payload accepted");world.players[0].connected=false;require(!validate_world(world,error),"Disconnected watcher state accepted");
}
#ifdef SSNE_TEST_SESSION_TRANSPORT
void wait_for(const std::function<bool()>& condition,const char* why){const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(6);while(!condition()){require(std::chrono::steady_clock::now()<deadline,why);std::this_thread::sleep_for(std::chrono::milliseconds(2));}}
void actor(WorldState& world,PlayerId player){EntityState entity;entity.id=100+player.slot;entity.kind=EntityKind::Player;entity.definition=2;entity.owner=player;entity.properties={{"position",std::array<float,3>{float(player.slot),0,0}},{"health",100.f},{"player_slot",int64_t(player.slot)}};world.entities[entity.id]=std::move(entity);}
void host_guard_test(){
    AuthoritativeServer server(4);require(server.start(0,"123456",98765,"Solo host"),"Solo host start failed");
    const auto host=server.status().player;WorldState world;world.epoch=1;world.tick=1;world.level="Rlevel1_1";actor(world,host);std::string error;
    require(server.publish(world,error) && server.attach_character(host,100+host.slot) && server.ready_host(),"Solo host readiness failed");
    require(!server.host_spectator_available() && server.host_spectator(1,true)==CommandResult::NotEligible && !server.participants()[host.slot].spectator,"Solo host entered voluntary spectator mode");
    const auto bot=server.add_bot();require(bool(bot),"Solo guard bot fixture failed");
    actor(world,*bot);++world.tick;require(server.publish(world,error) && server.attach_character(*bot,100+bot->slot),"Solo guard bot binding failed");
    require(!server.host_spectator_available() && server.host_spectator(2,true)==CommandResult::NotEligible,"A bot enabled voluntary host spectating");
    AuthoritativeClient client;require(client.start("127.0.0.1",server.status().port,"123456",98765,"Human"),"Solo guard human start failed");
    wait_for([&]{return client.status().connected;},"Solo guard human join timed out");const auto remote=client.status().player;
    actor(world,remote);++world.tick;require(server.publish(world,error) && server.attach_character(remote,100+remote.slot),"Solo guard human binding failed");client.ready(1);
    wait_for([&]{return server.participants()[remote.slot].ready;},"Solo guard human readiness timed out");
    require(server.host_spectator_available() && server.host_afk(3,true)==CommandResult::Applied && server.host_spectator(4,true)==CommandResult::Applied,"Connected human did not allow host spectating");
    client.set_spectator(true);wait_for([&]{return server.participants()[remote.slot].spectator;},"Remote spectator preference fixture failed");
    const auto host_bit=uint8_t(1u<<host.slot);require(server.set_spectator_lock(host_bit),"Epoch lock fixture failed");
    ControlInput held;held.pad.buttons=ssne::PAD_START|ssne::PAD_R;held.pad.trigger_r=255;held.yaw=100000;held.pressed=ssne::PAD_R;server.submit_host_input(held);
    world.epoch=2;world.tick=1;world.level="Rlevel2_1";
    require(server.publish(world,error,true),error.c_str());const auto loading=server.participants();
    require(loading[host.slot].afk && loading[host.slot].spectator && loading[remote.slot].spectator,"Campaign transition discarded AFK or remote voluntary spectator preference");
    require(!loading[host.slot].ready && !loading[host.slot].character && server.latest_world()->mission.at("Session.Spectators.LockMask")==Property(int64_t(0)),"Campaign transition retained old actor readiness or forced death lock");
    ControlInput local_ui;server.take_controls(&local_ui);require(!local_ui.pad.buttons && !local_ui.pad.trigger_r && local_ui.yaw==0 && !local_ui.pressed,"Campaign transition replayed old host menu/fire input");
    require(server.attach_character(host,100+host.slot) && server.attach_character(remote,100+remote.slot) && server.ready_host(),"Host did not become ready after epoch replacement");client.ready(2);
    wait_for([&]{return server.participants()[remote.slot].ready;},"Human did not become ready after epoch replacement");
    client.stop();wait_for([&]{return !server.participants()[remote.slot].connected;},"Last human did not disconnect");
    const auto solo=server.participants()[host.slot];require(!solo.spectator && solo.afk && !server.host_spectator_available() && server.host_spectator(5,true)==CommandResult::NotEligible,"Last human departure left host watching bots or discarded AFK preference");
    require(server.set_spectator_lock(host_bit) && server.participants()[host.slot].spectator && !server.cancel_spectator_activation(host) && server.host_spectator(6,false)==CommandResult::NotEligible,"Solo guard escaped forced Survival elimination");
    world.epoch=3;world.tick=1;require(server.publish(world,error,true),error.c_str());require(!server.participants()[host.slot].spectator && server.participants()[host.slot].afk,"Restart retained forced spectator death or lost AFK preference");
    require(server.remove_bot(*bot),"Solo guard bot cleanup failed");server.stop();
}
void tcp_test(){
    AuthoritativeServer server(4);require(server.start(0,"123456",98765,"Host"),"Host start failed");AuthoritativeClient client;require(client.start("127.0.0.1",server.status().port,"123456",98765,"Client"),"Client start failed");wait_for([&]{return client.status().connected;},"Client join timed out");const auto host=server.status().player,remote=client.status().player;
    WorldState world;world.epoch=1;world.tick=1;world.level="DM01";actor(world,host);actor(world,remote);std::string error;require(server.publish(world,error),error.c_str());require(server.attach_character(host,100+host.slot) && server.attach_character(remote,100+remote.slot) && server.ready_host(),"Native bindings failed");client.ready(1);wait_for([&]{return server.take_controls()[remote.slot].ready;},"Client readiness timed out");
    require(server.host_spectator(1,true)==CommandResult::Applied,"Host watch command failed");client.set_spectator(true);wait_for([&]{return server.participants()[remote.slot].spectator && client.latest_world()->players[host.slot].spectator && client.latest_world()->players[remote.slot].spectator;},"Effective spectators not replicated");
    ControlInput raw;raw.pad.buttons=ssne::PAD_START|ssne::PAD_RIGHT|ssne::PAD_L|ssne::PAD_R;raw.pad.trigger_l=raw.pad.trigger_r=255;raw.pad.stick_x=230;raw.pad.cstick_x=255;raw.yaw=100000;raw.pitch=90000;raw.weapon=3;raw.preferences.fill(1);server.submit_host_input(raw);client.submit_input(raw);
    PlayerControl got;ControlInput host_ui;wait_for([&]{const auto controls=server.take_controls(&host_ui);got=controls[remote.slot];return got.sequence && (got.input.pad.buttons&ssne::PAD_RIGHT);},"Spectator cycle input missing");
    require(got.spectator && !got.afk && !got.input.pad.trigger_l && !got.input.pad.trigger_r && got.input.pad.cstick_x==128 && got.input.yaw==0 && got.input.pitch==0 && got.input.weapon==255 && !(got.input.pad.buttons&(ssne::PAD_L|ssne::PAD_R)),"Remote spectator leaked gameplay input");require((host_ui.pad.buttons&ssne::PAD_START) && !host_ui.pad.trigger_r && host_ui.yaw==0,"Spectator Host lost sanitized Pause input");
    require(!server.cancel_spectator_activation({remote.slot,remote.generation+1}) && server.participants()[remote.slot].spectator,"Server denial accepted stale generation");
    require(server.cancel_spectator_activation(remote),"Server could not deny native activation");wait_for([&]{return !client.latest_world()->players[remote.slot].spectator && !server.take_controls()[remote.slot].spectator;},"Canceled voluntary watch did not publish to client/controls");
    client.set_spectator(true);wait_for([&]{return server.participants()[remote.slot].spectator;},"Fresh spectator intent after native denial rejected");
    const auto bit=uint8_t(1u<<remote.slot);require(server.set_spectator_lock(bit),"Round lock rejected");require(!server.cancel_spectator_activation(remote) && server.participants()[remote.slot].spectator,"Server native denial escaped forced round lock");const auto lock_revision=server.status().world_revision;wait_for([&]{return client.latest_world()->revision>=lock_revision;},"Lock snapshot not received");const auto received=client.status().bytes_received;client.set_spectator(false);client.request_snapshot();wait_for([&]{return client.status().bytes_received>received+512;},"Ordered post-command snapshot missing");require(server.participants()[remote.slot].spectator && (server.voluntary_spectator_mask()&bit),"Locked leave changed voluntary state");require(server.set_spectator_lock(0) && server.participants()[remote.slot].spectator,"Round unlock cleared voluntary watcher");
    client.set_spectator(false);wait_for([&]{return !server.participants()[remote.slot].spectator;},"Fresh voluntary leave rejected");require(server.set_spectator_lock(bit) && server.participants()[remote.slot].spectator && !(server.voluntary_spectator_mask()&bit),"Forced elimination became voluntary watch");require(server.set_spectator_lock(0) && !server.participants()[remote.slot].spectator,"Round restart did not release eliminated active player");
    require(server.set_spectator_lock(bit),"Leave fixture lock failed");client.stop();wait_for([&]{return !server.participants()[remote.slot].connected;},"Client leave timed out");AuthoritativeClient next;require(next.start("127.0.0.1",server.status().port,"123456",98765,"Replacement"),"Replacement start failed");wait_for([&]{return next.status().connected;},"Replacement join timed out");require(next.status().player.slot==remote.slot && next.status().player.generation!=remote.generation && !server.participants()[remote.slot].spectator,"Slot reuse inherited forced spectator");next.stop();server.stop();
}
#endif
}

int main() {
    try {
        authority_test();
        controller_test();
        codec_test();
#ifdef SSNE_TEST_SESSION_TRANSPORT
        host_guard_test();
        tcp_test();
#endif
        std::cout << "Spectator checks passed: " << checks << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
