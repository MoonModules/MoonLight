#pragma once

#include "core/util/ClockSync.h"
#include "platform/platform_clock.h"   // uptimeUs
#include "platform/platform_tasks.h"   // WorkerTask
#include "platform/platform_net.h"   // UdpSocket

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace mm {

namespace osc { class Reader; }

/// Keeps this device's fleet time on the reference's: the NTP exchange by unicast OSC, on a socket and a task of its own.
///
/// @technical
///
/// **One fleet, one reference.** The reference is the lowest device id (the low four bytes of the MAC) among the live devices heard running the clock, so every device picks the same one, unelected; a device that is the lowest keeps its own fleet time.
///
/// **Heard running the clock.** A device holding fleet time multicasts so every sixteen seconds, its fleet time in the beacon, so only devices running the clock are asked, and one restarting drops out for under a minute and comes back as it was.
///
/// **The fleet's time is inherited.** A device that boots listens for one beacon interval before it beacons or claims: hearing the fleet, it takes the newest fleet time heard; hearing no one, it claims its own. So a reboot of the lowest device, which becomes the reference again, carries the fleet's time on instead of stepping every device back to its uptime.
///
/// **Timestamps as the datagram arrives.** The task blocks on the socket and stamps t2 and t4 the moment it wakes, and t3 as the reply goes out; a stamp taken at a 20 ms poll would be up to 20 ms late. A board with no tasks polls every 20 ms, less precise.
///
/// **Asked often, then rarely.** Once a second until the filter holds eight samples, then every sixteen seconds, as NTP's poll interval backs off; a new reference starts over.
///
/// **Every device answers.** A request is answered with this device's fleet time whoever asks, to the address and port it came from, since any device may become the reference and a tool may measure the fleet.
class FleetClock {
public:
    /// The port every device answers on.
    static constexpr uint16_t kPort = 65511;
    /// The OSC address of a request, its argument t1.
    static constexpr const char* kAskAddress = "/moonlight/clock";
    /// The OSC address of a reply, its arguments t1, t2 and t3.
    static constexpr const char* kReplyAddress = "/moonlight/clock/reply";
    /// The OSC address of the beacon that says a device runs the clock, its arguments the device's id and its fleet time.
    static constexpr const char* kHereAddress = "/moonlight/clock/here";
    /// The multicast group the beacon goes to, beside the groups' .79.
    static constexpr uint8_t kGroup[4] = {239, 255, 77, 80};
    /// How often a device says it runs the clock.
    static constexpr int64_t kHereEveryUs = 16000000;
    /// How long a device counts as running the clock after its last beacon: three beacons.
    static constexpr uint32_t kHereStaleMs = 50000;
    /// How long a device that boots listens for the fleet's beacons before it claims its own time: a beacon interval and a margin.
    static constexpr int64_t kListenUs = kHereEveryUs + 2000000;

    /// A clock on no network yet: the first tick opens it.
    FleetClock() = default;
    /// Stops the task before the clock it reads goes, however its owner ends.
    virtual ~FleetClock() { close(); }
    /// Not copied: the task holds this clock's address.
    FleetClock(const FleetClock&) = delete;
    /// Not assigned, for the same reason.
    FleetClock& operator=(const FleetClock&) = delete;

    /// Once a second, from Discovery: the reference is the lowest of `selfId` and the ids `forEachPeer` offers, each with its address and name; start the task once the network is up.
    template <class ForEachPeer>
    void tick(uint32_t selfId, ForEachPeer forEachPeer) MM_NONBLOCKING {
        selfId_.store(selfId, std::memory_order_relaxed);
        uint32_t bestId = selfId, bestIp = 0;
        const char* bestName = nullptr;
        forEachPeer([&](uint32_t id, const uint8_t ip[4], const char* name) {
            if (id >= bestId || !runsClock(id)) return;
            bestId = id;
            bestIp = packIp(ip);
            bestName = name;
        });
        choose(bestId == selfId ? 0 : bestId, bestIp, bestName);
    }

    /// Start the task, which opens the socket once the network is up; a board with no tasks opens it from poll().
    void start();

    /// Every 20 ms on a board with no tasks: what the task does, returning at once.
    void poll();

    /// Stop the task and close the socket.
    void close();

    /// What a group's status adds about the clock: the reference and the spread, or that it syncs.
    const char* note() const MM_NONBLOCKING;

    /// Handle one datagram from `srcIp`:`srcPort`, stamped `arrivedUs` on the local clock as it arrived: what a test drives directly.
    void handle(const uint8_t* pkt, size_t len, const uint8_t srcIp[4], uint16_t srcPort, int64_t arrivedUs);

    /// Send a request now, stamped `t1Us` on the local clock, when a reference is chosen: what a test drives directly.
    void ask(int64_t t1Us);

    /// The reference's id, 0 when this device is the reference.
    uint32_t referenceId() const { return refId_.load(std::memory_order_acquire); }

protected:
    /// Send one datagram to `ip`:`port`, the seam a test captures.
    virtual void transmit(const uint8_t ip[4], uint16_t port, const uint8_t* data, size_t len);
    /// The local clock, the seam a test drives.
    virtual int64_t localUs() const { return static_cast<int64_t>(platform::uptimeUs()); }
    /// Open the socket and start the task once the network is up, the seam a test replaces so no case touches the network.
    virtual bool ensureOpen();
    /// This device's published fleet time, the seam a test gives each of its clocks.
    virtual ClockDiscipline published() const { return FleetTime::current(); }
    /// Publish the steered discipline for every task, the same seam.
    virtual void publish(const ClockDiscipline& d) { FleetTime::publish(d); }

private:
    void choose(uint32_t id, uint32_t ip, const char* name) MM_NONBLOCKING;
    /// Whether device `id` said it runs the clock within kHereStaleMs.
    bool runsClock(uint32_t id) const MM_NONBLOCKING;
    /// The slot that keeps device `id`: its own, else an empty or stale one, else the highest id's when `id` is lower, else kHeard.
    uint8_t slotFor(uint32_t id, uint32_t nowMs) const;
    void heard(uint32_t id, int64_t fleetUs, int64_t arrivedUs);
    void beacon();
    void runLoop();
    /// Read what is pending and send a request when one is due; `blockMs` waits for a datagram first.
    void service(uint32_t blockMs);
    void reply(const uint8_t srcIp[4], uint16_t srcPort, int64_t t1, int64_t arrivedUs);
    /// A reply to the request in flight: its sample into the filter, the discipline steered to the best.
    void takeReply(osc::Reader& r, const uint8_t srcIp[4], int64_t arrivedUs);
    /// Start the filter over when the reference changed since the last request.
    void startOverOnNewReference();
    /// As the reference while unsynced: the fleet's time heard in a beacon, else after listening this device's own.
    void takeFleetTime(int64_t nowUs);
    static uint32_t packIp(const uint8_t ip[4]) { return (uint32_t{ip[0]} << 24) | (uint32_t{ip[1]} << 16) | (uint32_t{ip[2]} << 8) | ip[3]; }
    static void unpackIp(uint32_t v, uint8_t ip[4]) { ip[0] = v >> 24; ip[1] = v >> 16; ip[2] = v >> 8; ip[3] = v; }
    static void trampoline(void* self) { static_cast<FleetClock*>(self)->runLoop(); }

    platform::UdpSocket sock_;
    platform::WorkerTask task_;
    std::atomic<bool> stop_{false};
    bool open_ = false;
    std::atomic<uint32_t> refId_{0};   ///< 0: this device is the reference
    std::atomic<uint32_t> refIp_{0};
    std::atomic<uint32_t> refGen_{0};   ///< counts reference changes, so the task starts its filter over
    std::atomic<uint32_t> selfId_{0};   ///< this device's id, which its beacon carries
    char refName_[32] = "";
    bool chosen_ = false;   ///< a reference was chosen, so the note has something to say
    uint32_t gen_ = 0;   ///< the reference change the task last saw; the task owns this and what follows: the exchange in flight, the filter, the discipline
    int64_t pendingT1_ = 0;
    int64_t nextAskUs_ = 0;
    uint8_t answered_ = 0;
    int64_t nextHereUs_ = 0;
    int64_t startedUs_ = -1;    ///< the local time the task first ran, from which a booting device listens
    int64_t heardFleetUs_ = 0;  ///< the newest fleet time a beacon carried, and the local time it arrived, which an unsynced device takes
    int64_t heardAtUs_ = -1;
    ClockFilter filter_;
    ClockDiscipline discipline_;
    std::atomic<int32_t> spreadUs_{-1};   ///< half the best round trip, the offset's error bound; -1 before a sample
    static constexpr uint8_t kHeard = 32;   ///< how many devices heard running the clock are kept, by id and when: the task writes them, the tick reads
    std::atomic<uint32_t> heardId_[kHeard] = {};
    std::atomic<uint32_t> heardMs_[kHeard] = {};
    mutable char note_[64] = "";
};

}  // namespace mm
