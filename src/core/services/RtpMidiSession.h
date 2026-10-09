#pragma once

#include "core/util/RtpMidi.h"
#include "platform/platform.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mm {

/// One RTP-MIDI session with one peer, over a control socket and a data socket on the port above it.
///
/// It accepts an invitation, or invites a host itself and asks again until that host answers.
/// The inviting side keeps a clock sync going, which is also how each side knows the other is still there.
/// A session that hears nothing for a while ends, and an inviter then invites again.
/// Messages to send gather in one packet that `flush` sends, so a desk's state goes out in a few datagrams rather than one per message.
class RtpMidiSession {
public:
    /// Where the session stands.
    enum class State : uint8_t {
        Closed,      ///< no sockets
        Listening,   ///< waiting to be invited
        Inviting,    ///< asking a host, at its control port or then its data port
        Connected,   ///< a peer on both ports
    };

    /// A closed session, opened by `open`.
    RtpMidiSession() = default;
    /// Say goodbye to the peer on the way out.
    ~RtpMidiSession() { close(); }
    /// Sessions are not copied; each owns its two sockets.
    RtpMidiSession(const RtpMidiSession&) = delete;
    /// Sessions are not copy-assigned, for the same reason.
    RtpMidiSession& operator=(const RtpMidiSession&) = delete;

    /// Bind `port` for control and the next one for data, introducing this side as `name`; false when either is taken.
    bool open(uint16_t port, const char* name) {
        close();
        std::snprintf(name_, sizeof(name_), "%s", name && name[0] ? name : "MoonLight");
        if (port == 0 || port == 65535) return false;   // the data port is the one above, which must exist
        if (!control_.open() || !control_.bind(port) || !data_.open() || !data_.bind(static_cast<uint16_t>(port + 1))) {
            control_.close();
            data_.close();
            return false;
        }
        ssrc_ = randomId();
        state_ = State::Listening;
        return true;
    }

    /// Say goodbye to the peer, if there is one, and free both sockets.
    void close() {
        if (state_ == State::Connected) sendExchange(rtpmidi::Command::End, token_, peerControlPort_, control_);
        control_.close();
        data_.close();
        state_ = State::Closed;
        inviting_ = false;
        writer_ = rtpmidi::DataWriter(tx_, sizeof(tx_));
    }

    /// Invite the host at `ip` and control `port`, asking again until it accepts, and again whenever the session ends.
    void invite(const uint8_t ip[4], uint16_t port) {
        if (inviting_ && std::memcmp(inviteIp_, ip, 4) == 0 && invitePort_ == port) return;
        if (state_ == State::Connected) endSession();
        std::memcpy(inviteIp_, ip, 4);
        invitePort_ = port;
        inviting_ = true;
        startInviting();
    }

    /// Stop inviting, ending a session this side invited; invitations from others are accepted again.
    void stopInviting() {
        if (!inviting_) return;
        inviting_ = false;
        if (state_ == State::Connected || state_ == State::Inviting) endSession();
    }

    /// Refuse invitations until accepting is turned back on, saying `why`, and end a session a peer invited; a bridge with no desk does this.
    void setAccepting(bool on, const char* why = "closed") {
        accepting_ = on;
        notAccepting_ = why;
        if (!on && !inviting_ && state_ == State::Connected) endSession();
    }

    /// Read what arrived, answer the handshake and the clock sync, keep the session alive, and call `onMessage(msg, len)` per MIDI message.
    template <class F>
    void service(F&& onMessage) {
        if (state_ == State::Closed) return;
        const uint32_t now = platform::millis();
        uint8_t ip[4];
        uint16_t port = 0;
        int n = 0;
        for (int i = 0; i < kMaxPerService && (n = control_.recvFrom(rx_, sizeof(rx_), ip, &port)) > 0; i++)
            onControl(static_cast<size_t>(n), ip, port, now);
        for (int i = 0; i < kMaxPerService && (n = data_.recvFrom(rx_, sizeof(rx_), ip, &port)) > 0; i++)
            onData(static_cast<size_t>(n), ip, port, now, onMessage);
        keepAlive(now);
        flush();
    }

    /// Queue one complete MIDI message for the peer; false when there is no session.
    bool send(const uint8_t* msg, size_t len) {
        if (state_ != State::Connected) return false;
        if (!writer_.add(msg, len)) {
            flush();
            return writer_.add(msg, len);
        }
        return true;
    }

    /// Send what `send` queued.
    void flush() {
        const size_t len = writer_.finish(seq_, timestamp(), ssrc_);
        writer_ = rtpmidi::DataWriter(tx_, sizeof(tx_));
        if (!len || state_ != State::Connected) return;
        seq_++;
        data_.sendToAddr(peerIp_, peerDataPort_, tx_, len);
    }

    /// Where the session stands.
    State state() const { return state_; }
    /// Whether a peer is on both ports.
    bool connected() const { return state_ == State::Connected; }
    /// The peer's address, valid while connected.
    const uint8_t* peerIp() const { return peerIp_; }
    /// The name the peer introduced itself with.
    const char* peerName() const { return peerName_; }
    /// Whether the last invitation was refused, which the owner reports.
    bool refused() const { return refused_; }
    /// Why the host refused, in its own words, empty when it said nothing.
    const char* refusal() const { return refusal_; }

private:
    static constexpr size_t kMaxPacket = 512;         ///< a desk's whole state fits a few of these
    static constexpr int kMaxPerService = 16;         ///< datagrams read per socket per call
    static constexpr uint32_t kInviteEveryMs = 1000;  ///< how often an unanswered invitation is sent again
    static constexpr uint32_t kSyncEveryMs = 10000;   ///< how often the inviter syncs the clock
    static constexpr uint32_t kSilentMs = 30000;      ///< how long an inviter's session lasts with nothing heard
    static constexpr uint32_t kHalfOpenMs = 3000;     ///< how long a half-open session holds off a second peer

    /// The session's clock, in RTP-MIDI's 100-microsecond units.
    uint64_t clock() {
        const uint32_t ms = platform::millis();
        clockMs_ += ms - lastMs_;   // wraps safely: the difference of two 32-bit counts
        lastMs_ = ms;
        return clockMs_ * 10;
    }
    uint32_t timestamp() { return static_cast<uint32_t>(clock()); }

    static uint32_t randomId() {
        static uint32_t seed = 0;
        seed = seed * 1664525u + 1013904223u + platform::micros();   // a fresh value per call; uniqueness, not secrecy
        return seed ? seed : 1;
    }

    void sendExchange(rtpmidi::Command c, uint32_t token, uint16_t port, platform::UdpSocket& sock, const uint8_t* ip = nullptr, const char* name = nullptr) {
        uint8_t pkt[64];
        const size_t len = rtpmidi::encodeExchange(pkt, sizeof(pkt), c, token, ssrc_, name ? name : name_);
        if (len) sock.sendToAddr(ip ? ip : peerIp_, port, pkt, len);
    }

    void sendSync(uint8_t count, const uint64_t ts[3]) {
        rtpmidi::Sync s;
        s.ssrc = ssrc_;
        s.count = count;
        for (int i = 0; i < 3; i++) s.ts[i] = ts[i];
        uint8_t pkt[rtpmidi::kSyncLen];
        if (rtpmidi::encodeSync(pkt, sizeof(pkt), s)) data_.sendToAddr(peerIp_, peerDataPort_, pkt, sizeof(pkt));
    }

    void startInviting() {
        state_ = State::Inviting;
        onDataPort_ = false;
        refused_ = false;   // a new host has not answered yet
        token_ = randomId();
        lastInviteMs_ = platform::millis() - kInviteEveryMs;   // the first goes out on the next service
    }

    /// End the current session, then invite again or listen.
    void endSession() {
        if (state_ == State::Connected) sendExchange(rtpmidi::Command::End, token_, peerControlPort_, control_);
        // Forgotten, so a late data-port invitation from the old peer cannot reopen the session.
        peerName_[0] = 0;
        std::memset(peerIp_, 0, sizeof(peerIp_));
        peerControlPort_ = peerDataPort_ = 0;
        token_ = 0;
        writer_ = rtpmidi::DataWriter(tx_, sizeof(tx_));
        if (inviting_) startInviting();
        else state_ = State::Listening;
    }

    /// Take the peer this exchange came from.
    void takePeer(const rtpmidi::Exchange& e, const uint8_t ip[4], uint16_t port) {
        std::memcpy(peerIp_, ip, 4);
        peerControlPort_ = port;
        peerSsrc_ = e.ssrc;
        std::memcpy(peerName_, e.name, sizeof(peerName_));
    }

    /// A session packet on the control port.
    void onControl(size_t len, const uint8_t ip[4], uint16_t port, uint32_t now) {
        rtpmidi::Exchange e;
        if (!rtpmidi::parseExchange(rx_, len, e)) return;
        // A peer is its address and port, so two programs on one computer are two peers.
        const bool fromPeer = std::memcmp(ip, peerIp_, 4) == 0 && port == peerControlPort_;
        switch (e.command) {
            case rtpmidi::Command::Invitation: onInvitation(e, ip, port, fromPeer, now); return;
            case rtpmidi::Command::Accept: onAccept(e, ip, port, now); return;
            case rtpmidi::Command::Reject: onReject(e); return;
            case rtpmidi::Command::End: if (fromPeer) onEnd(e); return;
            default: return;
        }
    }

    /// The invited host said no, its name field saying why when it is a MoonLight device.
    void onReject(const rtpmidi::Exchange& e) {
        if (state_ != State::Inviting || e.token != token_) return;
        refused_ = true;
        std::memcpy(refusal_, e.name, sizeof(refusal_));
    }

    /// The peer said goodbye, so no goodbye goes back.
    void onEnd(const rtpmidi::Exchange& e) {
        if (e.ssrc != peerSsrc_ || state_ != State::Connected) return;
        state_ = State::Listening;
        endSession();
    }

    /// An invitation on the control port: one peer at a time, the same address and port inviting again being that peer restarting.
    void onInvitation(const rtpmidi::Exchange& e, const uint8_t ip[4], uint16_t port, bool fromPeer, uint32_t now) {
        // Between a peer's two invitations its session is half open, and a second peer waits until it completes or lapses.
        const bool halfOpen = peerControlPort_ != 0 && now - lastHeardMs_ < kHalfOpenMs;
        const bool busy = (state_ == State::Connected || halfOpen) && !fromPeer;
        // A refusal says why in its name field, a free-text field every other implementation reads as a session name.
        const char* why = inviting_ ? "it invites" : !accepting_ ? notAccepting_ : busy ? "in use" : nullptr;
        if (why) { sendExchange(rtpmidi::Command::Reject, e.token, port, control_, ip, why); return; }
        sendExchange(rtpmidi::Command::Accept, e.token, port, control_, ip);
        takePeer(e, ip, port);
        token_ = e.token;
        state_ = State::Listening;   // connected once the data port is invited too
        lastHeardMs_ = now;
    }

    /// The invited host accepted on its control port, so its data port is asked next.
    void onAccept(const rtpmidi::Exchange& e, const uint8_t ip[4], uint16_t port, uint32_t now) {
        if (state_ != State::Inviting || onDataPort_ || e.token != token_ || std::memcmp(ip, inviteIp_, 4) != 0) return;
        takePeer(e, ip, port);
        onDataPort_ = true;
        refused_ = false;
        lastInviteMs_ = now - kInviteEveryMs;
    }

    /// A packet on the data port: the second half of the handshake, a clock sync, or MIDI.
    template <class F>
    void onData(size_t len, const uint8_t ip[4], uint16_t port, uint32_t now, F& onMessage) {
        const rtpmidi::Command c = rtpmidi::commandOf(rx_, len);
        // The peer's data port is learned from its invitation or its answer, which the token vouches for.
        const bool fromPeerIp = std::memcmp(ip, peerIp_, 4) == 0;
        if (c == rtpmidi::Command::Invitation || c == rtpmidi::Command::Accept) { onDataHandshake(c, len, ip, port, fromPeerIp, now); return; }
        if (!fromPeerIp || port != peerDataPort_ || state_ != State::Connected) return;
        lastHeardMs_ = now;   // anything from the peer means it is still there
        if (c == rtpmidi::Command::Sync) { onSync(len); return; }
        uint32_t ssrc = 0;
        if (c == rtpmidi::Command::None) rtpmidi::forEachMessage(rx_, len, ssrc, onMessage);
    }

    /// The data port's invitation, completing one the control port accepted, or the invited host's answer to ours.
    void onDataHandshake(rtpmidi::Command c, size_t len, const uint8_t ip[4], uint16_t port, bool fromPeerIp, uint32_t now) {
        if (len < 16) return;
        const uint32_t token = static_cast<uint32_t>(rtpmidi::getBE(rx_ + 8, 4));   // the token is all this step needs
        const bool ours = fromPeerIp && token == token_;
        if (c == rtpmidi::Command::Invitation) {
            if (!ours || inviting_) { sendExchange(rtpmidi::Command::Reject, token, port, data_, ip); return; }
            sendExchange(rtpmidi::Command::Accept, token, port, data_, ip);
        } else if (!ours || state_ != State::Inviting || !onDataPort_) {
            return;
        }
        peerDataPort_ = port;
        state_ = State::Connected;
        lastHeardMs_ = now;
        lastSyncMs_ = now - kSyncEveryMs;   // an inviter syncs at once
    }

    /// A clock sync step from the peer, answered with the next one.
    void onSync(size_t len) {
        rtpmidi::Sync s;
        if (!rtpmidi::parseSync(rx_, len, s) || s.count > 1) return;   // the third step needs no answer
        uint64_t ts[3] = {s.ts[0], s.ts[1], s.ts[2]};
        ts[s.count + 1] = clock();
        sendSync(static_cast<uint8_t>(s.count + 1), ts);
    }

    /// Invite again until answered, sync the clock while connected as the inviter, and end a session gone silent.
    void keepAlive(uint32_t now) {
        if (state_ == State::Inviting) { inviteAgain(now); return; }
        if (state_ != State::Connected) return;
        // The inviter hears an answer to its own sync; the invited side waits on the inviter's, which another implementation may send less often.
        if (now - lastHeardMs_ > (inviting_ ? kSilentMs : 3 * kSilentMs)) { endSession(); return; }   // with a goodbye, for a peer that is only slow
        if (!inviting_ || now - lastSyncMs_ < kSyncEveryMs) return;
        lastSyncMs_ = now;
        const uint64_t ts[3] = {clock(), 0, 0};
        sendSync(0, ts);
    }

    /// Send the pending invitation again once a second, to the control port or then the data port.
    void inviteAgain(uint32_t now) {
        if (now - lastInviteMs_ < kInviteEveryMs) return;
        lastInviteMs_ = now;
        if (!onDataPort_) { sendExchange(rtpmidi::Command::Invitation, token_, invitePort_, control_, inviteIp_); return; }
        sendExchange(rtpmidi::Command::Invitation, token_, static_cast<uint16_t>(invitePort_ + 1), data_, inviteIp_);
        // A host that accepted the control port but never the data port is asked from the start.
        if (++dataTries_ > 5) { dataTries_ = 0; onDataPort_ = false; }
    }

    platform::UdpSocket control_;
    platform::UdpSocket data_;
    State state_ = State::Closed;
    char name_[32] = {};
    char peerName_[32] = {};
    uint32_t ssrc_ = 0;
    uint32_t peerSsrc_ = 0;
    uint32_t token_ = 0;
    uint8_t peerIp_[4] = {};
    uint16_t peerControlPort_ = 0;
    uint16_t peerDataPort_ = 0;
    bool inviting_ = false;      ///< whether this side invites `inviteIp_`
    bool accepting_ = true;      ///< whether an invitation is answered
    bool onDataPort_ = false;    ///< whether the control port accepted and the data port is being invited
    bool refused_ = false;
    char refusal_[32] = {};      ///< why the invited host refused, from its `NO`
    const char* notAccepting_ = "closed";   ///< why this side refuses while not accepting
    uint8_t dataTries_ = 0;
    uint8_t inviteIp_[4] = {};
    uint16_t invitePort_ = rtpmidi::kDefaultPort;
    uint32_t lastInviteMs_ = 0;
    uint32_t lastSyncMs_ = 0;
    uint32_t lastHeardMs_ = 0;
    uint32_t lastMs_ = 0;
    uint64_t clockMs_ = 0;
    uint16_t seq_ = 0;
    uint8_t rx_[kMaxPacket] = {};   ///< the datagram being read
    uint8_t tx_[kMaxPacket] = {};   ///< the data packet being filled, which `flush` sends
    rtpmidi::DataWriter writer_{tx_, sizeof(tx_)};
};

}  // namespace mm
