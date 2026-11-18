#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
namespace port::coop::invite {
struct Signal {
    enum Kind { Offer,Answer,Cancel };
    Kind kind=Offer;
    std::string sender,target,description;
    uint64_t fingerprint=0;
};
// Empty target is valid for an Offer or an empty-description Cancel withdrawing
// that sender's pending Offer. The transport must restrict the latter to its
// matching pending, not-yet-usable peer; it cannot cancel established peers.
class Signalling {
public:
    using Callback=std::function<void(Signal)>;
    virtual ~Signalling()=default;
    // Callback runs on the receive worker; enqueue work, and call stop/destroy
    // from the owning thread. URL is the server base, topic is invitation_topic().
    // One subscription per instance: stop is terminal, even before start enters.
    // Obtain a fresh instance from the factory for a subsequent invitation.
    virtual bool start(const std::string& url,const std::string& topic,Callback,std::string& error)=0;
    // ntfy's default message limit applies to the full encoded JSON: 4096 bytes.
    // Larger offers fail explicitly; they are never uploaded as attachments.
    // Permanent payload failures start with "signal:oversize:" or "signal:invalid:";
    // transport/HTTP/subscription failures use neither prefix and may be retried.
    // Permanent payload failures start with "signal:oversize:" or "signal:invalid:";
    // transport/HTTP/subscription failures use neither prefix and may be retried.
    virtual bool publish(const Signal&,std::string& error)=0;
    // Subscription health only. EOF/errors reconnect on the receive worker;
    // false during that gap. No cached/replayed SDP: callers retry offers.
    virtual bool connected() const=0;
    virtual void stop()=0;
};
std::unique_ptr<Signalling> make_signalling();
}
