#include "authoritative_session.h"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <thread>
using namespace port::coop;
namespace {
void require(bool ok,const char* message){if(!ok){std::cerr<<message<<'\n';std::exit(1);}}
void wait_for(const std::function<bool()>& done,const char* message){
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!done()){require(std::chrono::steady_clock::now()<deadline,message);std::this_thread::sleep_for(std::chrono::milliseconds(2));}
}
bool reuse_ack(){const auto* value=std::getenv("SSNE_INPUT_ACK_REUSE");return !value || std::strcmp(value,"0");}
std::vector<uint8_t> body(const WorldState& input){auto copy=input;copy.revision=1;return encode_delta(difference(nullptr,copy));}
void add_player(WorldState& world,PlayerId who){
    EntityState e;e.id=100+who.slot;e.kind=EntityKind::Player;e.definition=2;e.owner=who;
    e.properties={{"position",std::array<float,3>{float(who.slot),0,0}},{"health",100.f},{"player_slot",int64_t(who.slot)}};
    world.entities[e.id]=std::move(e);
}
void attach(AuthoritativeServer& host,const WorldState& world){
    for(const auto& [id,e]:world.entities)if(e.kind==EntityKind::Player)require(host.attach_character(e.owner,id),"Character attachment failed");
    require(host.ready_host(),"Host readiness failed");
}
}
int main(){
    AuthoritativeServer host(4);require(host.start(0,"123456",1234,"Input host"),"Host start failed");
    AuthoritativeClient client;require(client.start("127.0.0.1",host.status().port,"123456",1234,"Input client"),"Client start failed");
    wait_for([&]{return client.status().connected;},"Client connection timeout");
    const auto local=host.status().player,remote=client.status().player;
    WorldState world;world.epoch=1;world.tick=1;world.level="Levels/Rome01";
    // Activate real per-peer event decoration, including idle same-revision polls.
    world.mission["Session.RocketEffects.Ids"]=std::vector<int64_t>{};
    world.mission["Session.RocketEffects.Styles"]=std::vector<int64_t>{};
    world.mission["Session.RocketEffects.Values"]=std::vector<float>{};
    add_player(world,local);add_player(world,remote);std::string error;
    require(host.publish(world,error),error.c_str());attach(host,world);client.ready(world.epoch);
    wait_for([&]{return host.take_controls()[remote.slot].ready;},"Remote readiness timeout");
    const auto ready_revision=host.latest_world()->revision;
    wait_for([&]{return client.latest_world()->revision>=ready_revision;},"Ready snapshot timeout");
    const auto captured=host.latest_world();const auto captured_body=body(*captured);
    require(captured->input_ack[local.slot]==0 && captured->input_ack[remote.slot]==0,"Unexpected initial input acknowledgement");
    ControlInput local_input;local_input.pad.stick_x=201;local_input.pad.buttons=ssne::PAD_A;local_input.yaw=12345;local_input.pitch=-5678;local_input.weapon=3;
    ControlInput remote_input;remote_input.pad.stick_y=219;remote_input.pad.buttons=ssne::PAD_R;remote_input.yaw=-34567;remote_input.pitch=7654;remote_input.weapon=4;
    host.submit_host_input(local_input);client.submit_input(remote_input);
    std::array<PlayerControl,max_players> first{};
    wait_for([&]{
        const auto controls=host.take_controls();
        for(const auto slot:{local.slot,remote.slot})if(controls[slot].sequence && !first[slot].sequence)first[slot]=controls[slot];
        return first[local.slot].sequence && first[remote.slot].sequence;
    },"Inputs were not immediately available to the native simulation");
    require(first[local.slot].ready && first[remote.slot].ready && first[local.slot].id==local && first[remote.slot].id==remote,"Input ownership/readiness changed");
    require(first[local.slot].input.yaw==12345 && first[local.slot].input.pitch==-5678 && first[local.slot].input.weapon==3 && first[local.slot].input.pad.stick_x==201,"Host controls changed");
    require(first[remote.slot].input.yaw==-34567 && first[remote.slot].input.pitch==7654 && first[remote.slot].input.weapon==4 && first[remote.slot].input.pad.stick_y==219,"TCP client controls changed");
    const auto before_publish=host.latest_world();
    require(body(*before_publish)==captured_body,"Input-only handling changed the published pose or acknowledged input");
    require(reuse_ack()?before_publish==captured:before_publish->revision>captured->revision,"Input snapshot reuse/reference mode mismatch");
    const auto drained=host.take_controls();
    for(const auto slot:{local.slot,remote.slot})require(!drained[slot].input.yaw && !drained[slot].input.pitch && !drained[slot].input.pressed && drained[slot].input.weapon==255,"Input transients were not drained exactly once");
    require(drained[local.slot].input.pad.stick_x==201 && drained[remote.slot].input.pad.stick_y==219,"Held controls were drained");
    require(host.latest_world()==before_publish,"Unchanged controls produced another snapshot");
    world.tick=2;world.entities.at(100+local.slot).properties["position"]=std::array<float,3>{10,0,0};
    world.entities.at(100+remote.slot).properties["position"]=std::array<float,3>{20,0,0};
    require(host.publish(world,error),error.c_str());const auto published=host.latest_world();
    require(published->input_ack[local.slot]==first[local.slot].sequence && published->input_ack[remote.slot]==first[remote.slot].sequence,"Capture omitted the newly consumed input acknowledgement");
    require(published->entities.at(100+remote.slot).properties==world.entities.at(100+remote.slot).properties,"Acknowledgement was not paired with the new captured pose");
    require(body(*captured)==captured_body,"Retained published snapshot was mutated");
    wait_for([&]{return client.latest_world()->tick==2;},"TCP pose/ack publication timeout");
    require(client.latest_world()->input_ack[remote.slot]==first[remote.slot].sequence,"TCP snapshot lost the pose acknowledgement");
    // A forced full snapshot must still bypass the same-revision fast path.
    const auto bytes=client.status().bytes_received;client.request_snapshot();
    wait_for([&]{return client.status().bytes_received>bytes+512;},"Same-revision forced resnapshot was suppressed");
    require(client.latest_world()->revision==published->revision && client.latest_world()->tick==2,"Forced resnapshot changed the captured state");
    // Event-consumer state has its own meaningful rebuild and delivery path.
    const auto event_revision=host.latest_world()->revision;client.acknowledge_native_events(1,NativeEventCursors{});
    wait_for([&]{return host.latest_world()->revision>event_revision;},"Native event acknowledgement did not publish its consumer update");
    const auto consumer_revision=host.latest_world()->revision;
    wait_for([&]{return client.latest_world()->revision>=consumer_revision;},"Updated event consumer snapshot was suppressed");
    require(body(*host.latest_world())==body(*published),"Event acknowledgement changed the captured pose or input acknowledgement");
    // Render polling can submit Space down/up before one authoritative tick.
    // The native jump action reads analog L, so the transient must retain both
    // halves of the trigger once, then release rather than repeating forever.
    ControlInput tap;tap.pad.buttons=ssne::PAD_L;tap.pad.trigger_l=255;
    for(unsigned i=0;i<8;++i){
        host.submit_host_input(tap);host.submit_host_input({});
        const auto jump=host.take_controls()[local.slot].input;
        require((jump.pad.buttons&ssne::PAD_L) && jump.pad.trigger_l==255 && !jump.pad.trigger_r && !(jump.pad.buttons&ssne::PAD_R),"Short host Space lost its native analog jump or became fire");
        const auto released=host.take_controls()[local.slot].input;
        require(!(released.pad.buttons&(ssne::PAD_L|ssne::PAD_R)) && !released.pad.trigger_l && !released.pad.trigger_r,"Consumed host Space pulse repeated after release");
    }
    client.submit_input(tap);client.submit_input({});
    // Chat is an ordered TCP barrier after the latest pending input packet.
    // Wait without consuming controls, so both press/release precede the tick.
    require(client.send_chat("jump-input-barrier"),"Remote input barrier rejected");
    wait_for([&]{for(const auto& message:host.chat_messages())if(message.text=="jump-input-barrier")return true;return false;},"Remote jump input barrier timeout");
    const auto remote_jump=host.take_controls()[remote.slot].input;
    require((remote_jump.pad.buttons&ssne::PAD_L) && remote_jump.pad.trigger_l==255 && !remote_jump.pad.trigger_r && !(remote_jump.pad.buttons&ssne::PAD_R),"Short client Space lost its native analog jump or became fire");
    const auto remote_release=host.take_controls()[remote.slot].input;
    require(!(remote_release.pad.buttons&(ssne::PAD_L|ssne::PAD_R)) && !remote_release.pad.trigger_l && !remote_release.pad.trigger_r,"Consumed client Space pulse repeated after release");
    ControlInput fire_tap;fire_tap.pad.buttons=ssne::PAD_R;fire_tap.pad.trigger_r=255;
    host.submit_host_input(fire_tap);host.submit_host_input({});
    const auto fire=host.take_controls()[local.slot].input;
    require((fire.pad.buttons&ssne::PAD_R) && fire.pad.trigger_r==255 && !fire.pad.trigger_l && !(fire.pad.buttons&ssne::PAD_L),"Short fire press became jump or lost its analog trigger");
    require(!host.take_controls()[local.slot].input.pad.trigger_r,"Consumed fire pulse repeated after release");
    // An event-based producer may observe release/repress between two samples
    // while the sampled analog value is still255. Preserve that explicit press
    // independently of held state; the native physics latch consumes the edge.
    // In particular, never synthesize a flight/held-jump release or a fire edge.
    ControlInput held_jump;held_jump.pad.buttons=ssne::PAD_L|ssne::PAD_R;
    held_jump.pad.trigger_l=held_jump.pad.trigger_r=255;held_jump.pad.stick_x=211;
    host.submit_host_input(held_jump);host.take_controls();
    auto repress=held_jump;repress.pressed=ssne::PAD_L;
    for(unsigned i=0;i<8;++i){
        host.submit_host_input(repress);host.submit_host_input(held_jump);
        const auto edge=host.take_controls()[local.slot].input;
        require((edge.pressed&ssne::PAD_L) && edge.pad.trigger_l==255 && edge.pad.trigger_r==255 && edge.pad.stick_x==211,"Explicit host jump re-press was merged into an unchanged hold");
        require(!(edge.pressed&ssne::PAD_R),"Host jump re-press repeated sustained fire");
        const auto held=host.take_controls()[local.slot].input;
        require(!held.pressed && held.pad.trigger_l==255 && held.pad.trigger_r==255,"Held host trigger was released or auto-repeated after one jump edge");
    }
    host.submit_host_input({});host.take_controls();
    ControlInput press_only;press_only.pressed=ssne::PAD_L;
    host.submit_host_input(press_only);host.submit_host_input({});
    auto explicit_tap=host.take_controls()[local.slot].input;
    require((explicit_tap.pressed&ssne::PAD_L) && explicit_tap.pad.trigger_l==255 && !explicit_tap.pad.trigger_r,"Fully released host event tap lost analog jump or became fire");
    require(!host.take_controls()[local.slot].input.pad.trigger_l,"Consumed host event-only jump repeated");
    client.submit_input(held_jump);require(client.send_chat("held-jump-start-barrier"),"Held remote jump barrier rejected");
    wait_for([&]{for(const auto& message:host.chat_messages())if(message.text=="held-jump-start-barrier")return true;return false;},"Held remote jump barrier timeout");
    require(host.take_controls()[remote.slot].input.pad.trigger_l==255,"Remote held jump fixture did not arrive");
    client.submit_input(repress);client.submit_input(held_jump);
    require(client.send_chat("held-jump-repress-barrier"),"Remote re-press barrier rejected");
    wait_for([&]{for(const auto& message:host.chat_messages())if(message.text=="held-jump-repress-barrier")return true;return false;},"Remote re-press barrier timeout");
    const auto remote_repress=host.take_controls()[remote.slot].input;
    require((remote_repress.pressed&ssne::PAD_L) && remote_repress.pad.trigger_l==255 && remote_repress.pad.trigger_r==255 && remote_repress.pad.stick_x==211,"Explicit client jump re-press was lost across client pending or server input merge");
    require(!(remote_repress.pressed&ssne::PAD_R),"Client jump re-press repeated sustained fire");
    const auto remote_held=host.take_controls()[remote.slot].input;
    require(!remote_held.pressed && remote_held.pad.trigger_l==255 && remote_held.pad.trigger_r==255,"Client held jump was released or auto-repeated after its event");
    // Membership/control transitions discard an already received jump edge.
    // These are the real server's authority paths, without a native mock.
    host.submit_host_input(press_only);require(host.host_afk(1,true)==CommandResult::Applied,"Host AFK jump-clear fixture rejected");
    require(!(host.take_controls()[local.slot].input.pressed&ssne::PAD_L),"AFK host retained a human jump edge");
    require(host.host_afk(2,false)==CommandResult::Applied && !(host.take_controls()[local.slot].input.pressed&ssne::PAD_L),"Returning host replayed an AFK jump edge");
    host.submit_host_input(press_only);require(host.host_spectator(3,true)==CommandResult::Applied,"Host watch jump-clear fixture rejected");
    require(!host.take_controls()[local.slot].input.pad.trigger_l,"Watcher retained a gameplay jump pulse");
    require(host.host_spectator(4,false)==CommandResult::Applied && !(host.take_controls()[local.slot].input.pressed&ssne::PAD_L),"Returning host replayed a watcher jump edge");
    host.submit_host_input(press_only);require(host.set_spectator_lock(uint8_t(1u<<local.slot)),"Forced watch jump-clear fixture rejected");
    require(host.set_spectator_lock(0) && !(host.take_controls()[local.slot].input.pressed&ssne::PAD_L),"Round unlock replayed an eliminated player's jump edge");
    client.set_afk(true);wait_for([&]{return host.participants()[remote.slot].afk && client.latest_world()->players[remote.slot].afk;},"AFK mutation no longer rebuilds");
    const auto before_remote_afk_input=host.take_controls()[remote.slot].sequence;
    ControlInput blocked;blocked.pad.stick_x=255;blocked.pad.buttons=ssne::PAD_R;blocked.pressed=ssne::PAD_L;client.submit_input(blocked);
    wait_for([&]{const auto c=host.take_controls()[remote.slot];return c.sequence>before_remote_afk_input && c.afk && c.input.pad.stick_x==128 && !c.input.pad.buttons;},"AFK human controls were not blocked");
    client.set_afk(false);wait_for([&]{return !host.participants()[remote.slot].afk && !client.latest_world()->players[remote.slot].afk;},"Return from AFK no longer rebuilds");
    require(!(host.take_controls()[remote.slot].input.pressed&ssne::PAD_L),"Returning client replayed a jump received while AFK");
    const auto bot=host.add_bot(2);require(bool(bot) && host.participants()[bot->slot].afk,"Bot admission lost automation");
    add_player(world,*bot);world.tick=3;require(host.publish(world,error),error.c_str());require(host.attach_character(*bot,100+bot->slot),"Bot attachment failed");
    const auto bot_control=host.take_controls()[bot->slot];require(bot_control.ready && bot_control.afk && bot_control.id==*bot,"Bot readiness/automation changed");
    AuthoritativeClient late;require(late.start("127.0.0.1",host.status().port,"123456",1234,"Late input client"),"Late client start failed");
    wait_for([&]{return late.status().connected;},"Late client connection timeout");const auto late_id=late.status().player;
    add_player(world,late_id);world.tick=4;require(host.publish(world,error),error.c_str());require(host.attach_character(late_id,100+late_id.slot),"Late character attachment failed");
    wait_for([&]{return late.latest_world()->entities.contains(100+late_id.slot);},"Late join world missing");late.ready(1);
    wait_for([&]{return host.take_controls()[late_id.slot].ready;},"Late join readiness depends on input rebuild");
    require(host.participants()[bot->slot].afk,"Late join reset bot automation");
    // A fresh epoch still invalidates bindings/readiness until explicit rejoin.
    host.submit_host_input(press_only);client.submit_input(press_only);
    require(client.send_chat("epoch-jump-clear-barrier"),"Epoch jump input barrier rejected");
    wait_for([&]{for(const auto& message:host.chat_messages())if(message.text=="epoch-jump-clear-barrier")return true;return false;},"Epoch jump input barrier timeout");
    world.epoch=2;world.tick=0;world.level="Levels/Rome02";require(host.publish(world,error),error.c_str());
    require(!host.take_controls()[local.slot].ready && !host.take_controls()[remote.slot].ready,"New epoch retained human readiness");
    wait_for([&]{return client.latest_world()->epoch==2 && late.latest_world()->epoch==2;},"New epoch not delivered");
    attach(host,world);client.ready(2);late.ready(2);
    wait_for([&]{const auto c=host.take_controls();return c[remote.slot].ready && c[late_id.slot].ready;},"Epoch readiness no longer rebuilds");
    const auto final_controls=host.take_controls();require(final_controls[bot->slot].ready && final_controls[bot->slot].afk,"Epoch reset bot automation");
    for(const auto slot:{local.slot,remote.slot})require(!(final_controls[slot].input.pressed&ssne::PAD_L) && !final_controls[slot].input.pad.trigger_l,"New epoch replayed a previous level jump edge");
    // Acknowledging the matching camera pose retires its local look delta.
    InputLookHistory prediction;prediction.reset(2);prediction.append(2,1,100000,-200000);
    const auto pending_look=prediction.sample(2,0,.01,{0,0,0},-1,1);
    require(std::abs(pending_look.yaw-.1)<1e-8 && std::abs(pending_look.pitch+.2)<1e-8,"Local input no longer leads the unacknowledged camera pose");
    const auto confirmed_look=prediction.sample(2,1,.02,{.1f,.2f,0},-1,1);
    require(confirmed_look.yaw==0. && confirmed_look.pitch==0.,"Matching pose/input acknowledgement retained a local look delta");
    late.stop();client.stop();host.stop();
    std::cout<<"Real TCP input publication: immediate controls, immutable pose-bound acks, reuse/reference snapshots, forced resnapshot, event consumer, host/client short and unchanged-held jump events, flight/fire hold preservation, lifecycle edge sanitation, fire pulse, AFK, bots, late join and epoch readiness passed.\n";
}
