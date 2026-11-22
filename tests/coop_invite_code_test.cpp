#include "invite_code.h"
#include "invite_transport.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <set>
#include <stdexcept>

using namespace port::coop::invite;
namespace {
unsigned checks=0;
void check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
bool same(const Invitation& a,const Invitation& b){return a.format==b.format && a.pin==b.pin && a.room==b.room;}
bool short_layout(const std::string& code){
    constexpr char alphabet[]="0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    if(code.size()!=14 || code[4]!='-' || code[9]!='-')return false;
    for(unsigned i=0;i<code.size();++i)if(i!=4 && i!=9 && std::find(std::begin(alphabet),std::end(alphabet)-1,code[i])==std::end(alphabet)-1)return false;
    return true;
}
std::string raw_code(std::string text){text.erase(std::remove(text.begin(),text.end(),'-'),text.end());return text;}

void legacy_checks(){
    std::string error;Invitation sample;sample.pin="000000";
    for(unsigned n=0;n<10;++n)sample.room[n]=uint8_t(n);
    check(sample.format==InvitationFormat::Legacy,"Default object changed legacy format");
    const auto encoded=encode_invitation(sample);
    check(encoded=="SSNE-2000-0001-081G-8186-0W40-J7E0","Known legacy CRC/Base32 vector changed");
    const auto decoded=decode_invitation(encoded,error);
    check(decoded && same(*decoded,sample) && error.empty(),"Known legacy vector did not roundtrip");
    check(invitation_topic(sample)=="ssne-v1-00010203040506070809","Known legacy 80-bit topic changed");
    std::string friendly=" \t";for(char c:encoded){friendly+=c>='A'&&c<='Z'?char(c+32):c;friendly+=' ';}friendly+="\r\n";
    check(bool(decode_invitation(friendly,error)),"Legacy case and pasted whitespace rejected");
    auto changed=encoded;changed[0]='A';check(!decode_invitation(changed,error),"Bad legacy prefix accepted");
    for(size_t i=5;i<encoded.size();++i)if(encoded[i]!='-'){
        changed=encoded;changed[i]=changed[i]=='0'?'1':'0';check(!decode_invitation(changed,error),"Legacy single-symbol mutation escaped version/CRC checks");
    }
    check(!decode_invitation("SSNE-4000-E001-081G-8186-0W40-J3X1",error) && error.find("version")!=std::string::npos,"Valid-CRC unknown legacy version accepted");
    check(!decode_invitation("SSNE-3X14-0001-081G-8186-0W40-JGZD",error) && error.find("range")!=std::string::npos,"Valid-CRC overflowing legacy PIN accepted");
}

void compact_vectors(){
    // Independently computed with Python integer packing and hashlib.sha256.
    struct Vector {const char* pin;std::array<uint8_t,5> room;const char* code;const char* topic;};
    const std::array<Vector,4> vectors{{
        {"000000",{0,1,2,3,4},"0000-000G-40R4","ssne-v1-1ed39eb709c7930f0ee2"},
        {"123456",{1,0x23,0x45,0x67,0x89},"3RJ0-04HM-ASW9","ssne-v1-04107351fef20293b178"},
        {"999999",{255,255,255,255,255},"YGHZ-ZZZZ-ZZZZ","ssne-v1-c4b40efe42bd312ee3a4"},
        {"000007",{0,0,0,0,0},"0007-0000-0000","ssne-v1-235d33ab4015b27736b1"}
    }};
    std::string error;
    for(const auto& v:vectors){
        Invitation sample;sample.format=InvitationFormat::Compact;sample.pin=v.pin;std::copy(v.room.begin(),v.room.end(),sample.room.begin());
        const auto encoded=encode_invitation(sample);check(encoded==v.code && short_layout(encoded),"Known compact 4-4-4 vector changed");
        const auto decoded=decode_invitation(encoded,error);check(decoded && same(*decoded,sample) && error.empty(),"Compact vector did not canonically roundtrip");
        check(encode_invitation(*decoded)==encoded && invitation_topic(*decoded)==v.topic,"Compact decode/encode/topic identity changed");
        check(invitation_topic(sample)==v.topic,"Independent compact SHA256 topic vector changed");
        const auto raw=raw_code(encoded);check(raw.size()==12 && bool(decode_invitation(raw,error)),"Unseparated twelve-symbol code rejected");
        std::string friendly="\t \r\n";for(char c:encoded){friendly+=c>='A'&&c<='Z'?char(c+32):c;friendly+=' ';}friendly+="\v\f";
        const auto pasted=decode_invitation(friendly,error);check(pasted && same(*pasted,sample),"Compact lowercase/pasted whitespace changed identity");
        check(std::all_of(decoded->room.begin()+5,decoded->room.end(),[](uint8_t value){return !value;}),"Compact unused room bytes were not canonical zero");
    }
    const auto zero=decode_invitation("0000-0000-0000",error);
    check(zero && zero->pin=="000000" && encode_invitation(*zero)=="0000-0000-0000","Leading-zero compact identity was lost");
    check(!decode_invitation("ZZZZ-ZZZZ-ZZZZ",error) && error.find("range")!=std::string::npos,"Compact 20-bit PIN overflow accepted");
    for(const auto* invalid:{"0000-0000-000O","0000-0000-000I","0000-0000-000L","0000-0000-000U","0000-0000-000!"})
        check(!decode_invitation(invalid,error),"Non-Crockford compact symbol accepted");
    check(!decode_invitation(std::string(11,'0'),error) && !decode_invitation(std::string(13,'0'),error),"Wrong compact length accepted");
    check(!decode_invitation(std::string(129,' '),error),"Raw input size limit was removed");
}

void canonical_and_topic_checks(){
    std::string error;const auto base=decode_invitation("3RJ0-04HM-ASW9",error);check(bool(base),"Compact test identity unavailable");
    auto altered=*base;altered.room[5]=1;
    check(encode_invitation(altered).empty() && invitation_topic(altered).empty(),"Noncanonical unused room bytes were silently truncated");
    altered=*base;altered.pin="123457";check(invitation_topic(altered)!=invitation_topic(*base),"Compact topic ignores the PIN bits");
    altered=*base;altered.room[4]^=1;check(invitation_topic(altered)!=invitation_topic(*base),"Compact topic ignores the room nonce");
    altered=*base;altered.format=InvitationFormat::Legacy;check(invitation_topic(altered)!=invitation_topic(*base),"Compact topic lost format-domain separation");
    altered=*base;altered.format=static_cast<InvitationFormat>(3);check(encode_invitation(altered).empty() && invitation_topic(altered).empty(),"Unknown object format was accepted");
    // V2 intentionally has no checksum. A changed valid symbol may identify a
    // different room; it must never silently retain the original identity.
    const auto raw=raw_code(encode_invitation(*base));
    for(unsigned i=0;i<raw.size();++i){
        auto changed=raw;changed[i]=changed[i]=='0'?'1':'0';const auto decoded=decode_invitation(changed,error);
        if(decoded)check(!same(*decoded,*base) && raw_code(encode_invitation(*decoded))==changed && invitation_topic(*decoded)!=invitation_topic(*base),"Compact symbol change aliased the original room");
        else check(error.find("range")!=std::string::npos,"Valid compact alphabet was rejected for an invented checksum");
    }
    for(const auto* pin:{"","00000","0000000","-00000","00000a"," 00000"}){
        check(!make_invitation(pin,error) && !error.empty(),"Invalid legacy PIN generated an invitation");
        check(!make_compact_invitation(pin,error) && !error.empty(),"Invalid compact PIN generated an invitation");
        altered=*base;altered.pin=pin;check(encode_invitation(altered).empty() && invitation_topic(altered).empty(),"Invalid compact PIN was encoded or assigned a topic");
    }
}

void independent_random_checks(){
    std::set<std::string> legacy_codes,compact_codes,compact_topics;std::string error;
    for(unsigned n=0;n<128;++n){
        const auto legacy=make_invitation("246810",error);check(legacy && error.empty() && legacy->format==InvitationFormat::Legacy,"Legacy RNG API no longer produces legacy invitations");
        const auto code=encode_invitation(*legacy);const auto back=decode_invitation(code,error);check(back && same(*back,*legacy),"Random legacy identity did not roundtrip");legacy_codes.insert(code);
        const auto compact=make_compact_invitation("246810",error);check(compact && error.empty() && compact->format==InvitationFormat::Compact,"Compact RNG API failed");
        const auto short_code=encode_invitation(*compact);check(short_layout(short_code),"Generated compact code is not twelve 4-4-4 symbols");
        const auto decoded=decode_invitation(short_code,error);check(decoded && same(*decoded,*compact) && invitation_topic(*decoded)==invitation_topic(*compact),"Random compact identity/topic did not roundtrip");
        check(std::all_of(compact->room.begin()+5,compact->room.end(),[](uint8_t value){return !value;}),"Compact RNG populated unused room bytes");
        compact_codes.insert(short_code);compact_topics.insert(invitation_topic(*compact));
    }
    check(legacy_codes.size()==128 && compact_codes.size()==128 && compact_topics.size()==128,"Independent generated rooms collided");
}
struct StopGate {
    std::mutex mutex;
    std::condition_variable changed;
    unsigned entered=0,completed=0;
    bool released=false;
    bool timed_out=false;
    void release(){
        std::lock_guard lock(mutex);released=true;changed.notify_all();
    }
};
class StalledStopSignalling final : public Signalling {
    std::shared_ptr<StopGate> gate_;
public:
    explicit StalledStopSignalling(std::shared_ptr<StopGate> gate):gate_(std::move(gate)){}
    bool start(const std::string&,const std::string&,Callback,std::string&) override{return true;}
    bool publish(const Signal&,std::string&) override{return true;}
    bool connected() const override{return true;}
    void stop() override{
        std::unique_lock lock(gate_->mutex);
        ++gate_->entered;
        gate_->changed.notify_all();
        if(!gate_->changed.wait_for(lock,std::chrono::seconds(4),[&]{return gate_->released;}))
            gate_->timed_out=true;
        ++gate_->completed;
    }
};
void retirement_checks(){
    using namespace std::chrono_literals;
    const auto gate=std::make_shared<StopGate>();
    InvitationRetirement retirement;
    struct ReleaseOnFailure {
        std::shared_ptr<StopGate> gate;
        ~ReleaseOnFailure(){gate->release();}
    } release{gate};
    Config config;
    config.testing_loopback_candidates=true;
    config.signalling_factory=[gate]{return std::make_unique<StalledStopSignalling>(gate);};
    auto start_host=[&]{
        check(retirement.prepare(),"Retirement queue refused a new host with room available");
        auto host=std::make_unique<InviteTransport>();
        std::string error;
        check(host->host(27341,"246810",7,config,error),"Could not create the private invitation host");
        const auto end=std::chrono::steady_clock::now()+2s;
        while(host->status().phase==Phase::Preparing && std::chrono::steady_clock::now()<end)
            std::this_thread::sleep_for(1ms);
        check(host->status().phase==Phase::Ready,"Private host never completed signalling startup");
        return host;
    };
    retirement.retire(start_host());
    {
        std::unique_lock lock(gate->mutex);
        check(gate->changed.wait_for(lock,2s,[&]{return gate->entered!=0;}),
              "Retired host did not begin cleanup");
        check(gate->completed==0,"Retiring an invitation waited for the blocked signalling request");
    }
    auto new_host=start_host();
    check(new_host->status().phase==Phase::Ready,"Old invitation teardown changed the replacement host");
    retirement.retire(std::move(new_host));
    retirement.retire(start_host());
    retirement.retire(start_host());
    check(!retirement.prepare(),"Retirement queue allowed unbounded pending invitations");
    gate->release();
    retirement.drain();
    check(gate->completed==4 && !gate->timed_out,"Process shutdown did not drain all retired invitations");
    check(!retirement.prepare(),"Invitation worker reopened after shutdown");
}
}
int main(){try{
    legacy_checks();compact_vectors();canonical_and_topic_checks();independent_random_checks();retirement_checks();
    std::cout<<"PASS invitation checks="<<checks<<"; codecs, topics, independent RNG and bounded nonblocking retirement passed\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<'\n';return 1;}}
