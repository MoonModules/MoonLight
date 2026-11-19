/// The fleet clock's exchange, beacon and task: what FleetClock.h declares.

#include "core/system/FleetClock.h"

#include "core/services/OscPacket.h"
#include "core/util/format.h"

#include <cstring>

namespace mm {

namespace {
constexpr int64_t kFastAskUs = 1000000;    // once a second while the filter fills
constexpr int64_t kSlowAskUs = 16000000;   // then every sixteen seconds
constexpr uint32_t kWaitMs = 100;          // the longest the task blocks, and so the most a stop or a due request waits
}  // namespace

bool FleetClock::ensureOpen() {
    if (open_) return true;
    uint8_t local[4] = {};
    platform::localIPv4(local);
    if (!(local[0] | local[1] | local[2] | local[3])) return false;   // no network yet
    if (!sock_.open() || !sock_.bind(kPort)) { sock_.close(); return false; }
    sock_.joinMulticast(kGroup);   // the beacons
    open_ = true;
    return true;
}

void FleetClock::start() {
    if (task_.impl) return;
    stop_.store(false, std::memory_order_release);
    // Priority 7, as the receive tasks: the stamps are only as good as the wake, and the task blocks between its rare datagrams.
    platform::spawnPinnedTask(task_, "mmClock", &trampoline, this, 3072, 7, -1);
}

void FleetClock::close() {
    if (task_.impl) {
        stop_.store(true, std::memory_order_release);
        platform::stopPinnedTask(task_);
    }
    sock_.close();
    open_ = false;
}

void FleetClock::choose(uint32_t id, uint32_t ip, const char* name) MM_NONBLOCKING {
    if (id != refId_.load(std::memory_order_relaxed) || ip != refIp_.load(std::memory_order_relaxed)) {
        refIp_.store(ip, std::memory_order_relaxed);
        refId_.store(id, std::memory_order_release);
        refGen_.fetch_add(1, std::memory_order_release);
    }
    formatTo(refName_, sizeof(refName_), "%s", id ? (name && name[0] ? name : "?") : "");
    chosen_ = true;
}

bool FleetClock::runsClock(uint32_t id) const MM_NONBLOCKING {
    const uint32_t now = platform::millis();
    for (uint8_t i = 0; i < kHeard; i++)
        if (heardId_[i].load(std::memory_order_acquire) == id && now - heardMs_[i].load(std::memory_order_relaxed) <= kHereStaleMs) return true;
    return false;
}

uint8_t FleetClock::slotFor(uint32_t id, uint32_t nowMs) const {
    for (uint8_t i = 0; i < kHeard; i++) if (heardId_[i].load(std::memory_order_relaxed) == id) return i;
    for (uint8_t i = 0; i < kHeard; i++)
        if (!heardId_[i].load(std::memory_order_relaxed) || nowMs - heardMs_[i].load(std::memory_order_relaxed) > kHereStaleMs) return i;
    return kHeard;   // a fleet past kHeard clock devices: the newest wait for a slot to go stale
}

void FleetClock::heard(uint32_t id, int64_t fleetUs, int64_t arrivedUs) {
    if (!id) return;
    if (fleetUs > 0) { heardFleetUs_ = fleetUs; heardAtUs_ = arrivedUs; }   // the fleet's time, as of its arrival
    const uint32_t now = platform::millis();
    const uint8_t slot = slotFor(id, now);
    if (slot == kHeard) return;
    heardId_[slot].store(0, std::memory_order_release);   // emptied first, so a reader never pairs the old id with the new time
    heardMs_[slot].store(now, std::memory_order_release);
    heardId_[slot].store(id, std::memory_order_release);
}

void FleetClock::beacon() {
    if (!discipline_.synced()) return;   // a device says it runs the clock only once it holds the fleet's time
    uint8_t msg[48];
    osc::Writer w(msg, sizeof(msg), kHereAddress, ",ih");
    w.i32(selfId_.load(std::memory_order_relaxed));
    w.i64(static_cast<uint64_t>(discipline_.fleetAt(localUs())));
    transmit(kGroup, kPort, msg, w.size());
}

void FleetClock::runLoop() {
    while (!stop_.load(std::memory_order_acquire)) {
        if (ensureOpen()) service(kWaitMs);
        else platform::waitNotify(task_, kWaitMs * 10);   // the network not up yet: tried again each second
    }
}

void FleetClock::poll() {
    if (!task_.impl && ensureOpen()) service(0);
}

void FleetClock::service(uint32_t blockMs) {
    if (blockMs) {
        platform::UdpSocket* socks[1] = {&sock_};
        platform::UdpSocket::waitReadable(socks, 1, blockMs);
    }
    uint8_t pkt[96];
    uint8_t src[4];
    uint16_t srcPort = 0;
    for (int i = 0; i < 8; i++) {   // bounded, so a flood cannot hold the task
        const int64_t arrived = localUs();
        const int n = sock_.recvFrom(pkt, sizeof(pkt), src, &srcPort);
        if (n <= 0) break;
        handle(pkt, static_cast<size_t>(n), src, srcPort, arrived);
    }
    const int64_t now = localUs();
    if (startedUs_ < 0) startedUs_ = now;
    if (refGen_.load(std::memory_order_acquire) != gen_) nextAskUs_ = now;   // a new reference is asked at once, not at the old one's interval
    if (now >= nextHereUs_) {
        nextHereUs_ = now + kHereEveryUs;
        beacon();
    }
    if (now >= nextAskUs_) ask(now);
}

void FleetClock::ask(int64_t t1Us) {
    if (startedUs_ < 0) startedUs_ = t1Us;
    startOverOnNewReference();
    const uint32_t id = refId_.load(std::memory_order_acquire);
    const uint32_t ipv = refIp_.load(std::memory_order_relaxed);
    nextAskUs_ = t1Us + (answered_ >= ClockFilter::kSamples ? kSlowAskUs : kFastAskUs);
    if (!id || !ipv) {   // this device is the reference: its fleet time stands as it is, and is the fleet's
        pendingT1_ = 0;
        if (!id && !discipline_.synced()) takeFleetTime(t1Us);
        return;
    }
    uint8_t msg[48];
    osc::Writer w(msg, sizeof(msg), kAskAddress, ",h");
    w.i64(static_cast<uint64_t>(t1Us));
    uint8_t ip[4];
    unpackIp(ipv, ip);
    pendingT1_ = t1Us;
    transmit(ip, kPort, msg, w.size());
}

void FleetClock::startOverOnNewReference() {
    const uint32_t gen = refGen_.load(std::memory_order_acquire);
    if (gen == gen_) return;
    gen_ = gen;   // a new reference: its filter starts over, the discipline carries on
    filter_.clear();
    answered_ = 0;
    pendingT1_ = 0;
    spreadUs_.store(-1, std::memory_order_relaxed);
}

void FleetClock::takeFleetTime(int64_t nowUs) {
    if (heardAtUs_ >= 0) discipline_.steer(heardAtUs_, heardFleetUs_ - heardAtUs_);   // the fleet's time, inherited
    else if (nowUs - startedUs_ >= kListenUs) discipline_.claim();                   // no one heard: this device's time is the fleet's
    else return;                                                                       // still listening
    publish(discipline_);
    nextHereUs_ = nowUs;   // and says so at once
}

void FleetClock::handle(const uint8_t* pkt, size_t len, const uint8_t srcIp[4], uint16_t srcPort, int64_t arrivedUs) {
    osc::Reader here(pkt, len, kHereAddress);
    if (here.ok()) {
        const uint32_t id = here.i32();
        const int64_t fleet = static_cast<int64_t>(here.i64());
        if (here.ok()) heard(id, fleet, arrivedUs);
        return;
    }
    osc::Reader ask(pkt, len, kAskAddress);
    if (ask.ok()) {
        const int64_t t1 = static_cast<int64_t>(ask.i64());
        if (ask.ok()) reply(srcIp, srcPort, t1, arrivedUs);
        return;
    }
    osc::Reader r(pkt, len, kReplyAddress);
    if (r.ok()) takeReply(r, srcIp, arrivedUs);
}

void FleetClock::takeReply(osc::Reader& r, const uint8_t srcIp[4], int64_t arrivedUs) {
    const int64_t t1 = static_cast<int64_t>(r.i64()), t2 = static_cast<int64_t>(r.i64()), t3 = static_cast<int64_t>(r.i64());
    if (!r.ok() || !pendingT1_ || t1 != pendingT1_ || packIp(srcIp) != refIp_.load(std::memory_order_relaxed)) return;   // a stray, late or forged reply
    pendingT1_ = 0;
    if (!filter_.add(clockSample(t1, t2, t3, arrivedUs))) return;
    if (answered_ < 255) answered_++;
    ClockSample best;
    filter_.best(best);
    discipline_.steer(arrivedUs, best.offsetUs);
    publish(discipline_);
    spreadUs_.store(static_cast<int32_t>(best.delayUs / 2 < INT32_MAX ? best.delayUs / 2 : INT32_MAX), std::memory_order_relaxed);
}

void FleetClock::reply(const uint8_t srcIp[4], uint16_t srcPort, int64_t t1, int64_t arrivedUs) {
    const ClockDiscipline d = published();
    uint8_t msg[64];
    osc::Writer w(msg, sizeof(msg), kReplyAddress, ",hhh");
    w.i64(static_cast<uint64_t>(t1));
    w.i64(static_cast<uint64_t>(d.fleetAt(arrivedUs)));
    w.i64(static_cast<uint64_t>(d.fleetAt(localUs())));
    transmit(srcIp, srcPort, msg, w.size());
}

void FleetClock::transmit(const uint8_t ip[4], uint16_t port, const uint8_t* data, size_t len) {
    if (open_ && len) sock_.sendToAddr(ip, port, data, len);
}

const char* FleetClock::note() const MM_NONBLOCKING {
    if (!chosen_) return nullptr;
    const int32_t spread = spreadUs_.load(std::memory_order_relaxed);
    if (!refId_.load(std::memory_order_acquire)) formatTo(note_, sizeof(note_), "clock: the reference");
    else if (spread < 0) formatTo(note_, sizeof(note_), "clock: syncing to %s", refName_);
    else formatTo(note_, sizeof(note_), "clock: %s, ±%u.%u ms", refName_, static_cast<unsigned>(spread / 1000), static_cast<unsigned>(spread % 1000 / 100));
    return note_;
}

}  // namespace mm
