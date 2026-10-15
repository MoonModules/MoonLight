#pragma once

#include <cstdint>
#include <cstddef>  // size_t

namespace mm {

/// A sink a producer sends bytes to, without depending on the HTTP server that carries them.
///
/// @technical
///
/// ## Who implements it and who holds it
///
/// `HttpServerModule` implements the interface, and producers such as `PreviewDriver` hold a pointer to it rather than to the concrete server.
/// A light-domain producer therefore depends on nothing more than something it can send bytes to.
/// The interface is domain-neutral: what the bytes mean is the caller's business.
///
/// ## Why a send is resumable
///
/// `sendBufferedFrame` takes a payload that lives in a stable caller-owned buffer, so nothing is copied.
/// One WebSocket message is the `header`, copied because it is small and may be a stack local, followed by `body`.
/// `body` is a pointer the caller keeps stable until the send completes or is canceled.
/// The implementation drains it across transport-poll ticks, a bounded chunk per tick, returning on a socket that would block.
/// A large frame therefore stays off the caller's hot path.
/// The browser still sees one atomic message: resumable means delivered over wall-clock, not split into several messages.
///
/// ## The three calls that pace a link
///
/// | Call | What it does |
/// |------|--------------|
/// | `sendBufferedFrame` | begins a send. While one is in flight a new call is dropped, keeping the in-flight frame and rejecting the new one, which the caller reads as a busy link |
/// | `bufferedSendIdle` | true when no send is in flight, so a caller gating the next frame on it self-limits to what the link sustains |
/// | `cancelBufferedSend` | abandons the in-flight send at once, which the caller does before it frees or reallocates `body` on a geometry rebuild |
///
/// ## What a cancel costs a client
///
/// A client caught mid-message by a cancel is closed, the only honest exit once bytes are out.
/// It reconnects, and the generation bump primes it fresh.
/// `PreviewDriver` is the one user today, so every `/wsp` message rides this one paced path.
///
/// ## Inbound messages are opaque
///
/// The transport unmasks a client's frame, framing being its job, and hands the payload bytes to the registered sink; only the producer knows what they mean.
/// `onClientGone` fires when a client's slot closes or turns over.
/// A producer holding per-slot standing state, such as the preview's stride and frame rate request, drops it with the client.
/// Both fire on the transport's own thread, core 0 under the split.
/// A producer ticking elsewhere therefore stores single-byte fields its reader tolerates racing on, which is the lossy-channel rule.
///
/// ## Why the send lease exists
///
/// The lease gives exclusive access to the sender for a producer that does not run on the transport's own thread.
/// Under the multicore split the offloaded `PreviewDriver` ticks on core 1 while the transport drains, reaps and admits on core 0.
/// That is two producers, two cores, one preview socket set and one resumable send slot.
/// The control channel stays core-0-only and outside this lease.
/// A producer therefore brackets a whole message in the acquire and release pair, because arming a frame must not race the drain that is reading the slot.
///
/// The acquire never blocks, because the caller may be on the render or encode thread where blocking violates the hot path.
/// A false result means the transport is busy this instant, so the message is skipped rather than waited on.
/// Skipping is already the producer's back-off path, `PreviewDriver` dropping a slot whenever the link is behind.
/// A lost race therefore costs one frame at most.
/// A single-threaded transport may return true unconditionally: with one producer thread there is no race to prevent, and the pair is then free.
struct BinaryBroadcaster {
    /// Begin one resumable frame, `header` copied and `body` kept stable by the caller.
    virtual bool sendBufferedFrame(const uint8_t* header, size_t headerLen,
                                   const uint8_t* body, size_t bodyLen) = 0;
    /// True when no send is in flight, the gate a producer paces itself on.
    virtual bool bufferedSendIdle() const = 0;
    /// Abandon the in-flight send now, before the `body` buffer goes away.
    virtual void cancelBufferedSend() = 0;

    /// How many subscribers are listening, for a status line or a log rather than a branch.
    virtual int subscriberCount() const { return 0; }

    /// Where a transport delivers inbound client bytes and slot closures.
    struct ClientMessageSink {
        /// One client's payload bytes, whose meaning only the producer knows.
        virtual void onClientMessage(int slot, const uint8_t* payload, int len) = 0;
        /// A slot closed or turned over, so any standing state for it is dropped.
        virtual void onClientGone(int slot) = 0;
    protected:
        ~ClientMessageSink() = default;
    };
    /// Register the sink that receives inbound messages, or clear it with null.
    virtual void setClientMessageSink(ClientMessageSink* sink) { (void)sink; }

    /// Take the sender if it is free this instant, never blocking.
    virtual bool tryAcquireSend() = 0;
    /// Give the sender back, once the whole message is armed.
    virtual void releaseSend() = 0;

protected:
    ~BinaryBroadcaster() = default;  // not owned through this interface
};

/// An RAII bracket for the acquire and release pair, the same shape as mm::LockGuard.
class SendLease {
public:
    /// Take the lease, which is held only when the transport was free.
    explicit SendLease(BinaryBroadcaster* bc)
        : bc_(bc), held_(bc && bc->tryAcquireSend()) {}
    /// Releases the lease when it was held, so a whole message is one scope.
    ~SendLease() { if (held_) bc_->releaseSend(); }
    /// True when the lease is held, so `if (SendLease s{bc}; s)` guards one whole message.
    explicit operator bool() const { return held_; }
    /// Not copyable: two leases would release one acquisition twice.
    SendLease(const SendLease&) = delete;
    SendLease& operator=(const SendLease&) = delete;
private:
    BinaryBroadcaster* bc_;
    bool held_;
};

} // namespace mm
