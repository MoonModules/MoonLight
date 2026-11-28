#pragma once
/// The platform's network: Ethernet with its raw frames, WiFi station and access point, the WiFi coprocessor, mDNS, name lookup, HTTPS room, Improv provisioning and the UDP and TCP sockets.
///
/// One concern of the platform interface ([platform.h](platform.h) gathers them all), so a file includes the concerns it uses and an edit to one rebuilds its users alone.
///
/// @technical
///
/// ## Ethernet transmit can wedge
///
/// The driver's internal link state can diverge from both the PHY and our own flag: seen on an S31 under sustained transmit, with the link lost and no disconnect event.
///
/// A stop and start re-runs link negotiation, the only supported way back, and blocks for up to four seconds while autonegotiation polls the PHY.
///
/// The housekeeping tick calls it only where every frame is being refused anyway, so a stalled render loop for one tick stays invisible to a user.
///

#include "platform/platform_base.h"   // target flags, MM_NONBLOCKING and the compiler macros
#include <atomic>   // the sockets' counters and Improv's readiness flags
#include "platform/platform_clock.h"   // millis, which the sockets' timeouts read

namespace mm::platform {


/// Make the next bind or listen fail, so a test reaches that path; false restores. Hogging the port instead is not portable: Linux permits the overlapping bind under SO_REUSEADDR, and macOS lets a non-root process bind port 80.
void setTestBindFails(bool fail);
#ifndef ESP_PLATFORM   // host-test seams, which no ESP32 code calls
/// How a multicast join behaves in a host test: through the host's network, succeeding while bypassing it so the network's traffic stays out of the test, or failing as on a network with no multicast.
enum class TestJoin : uint8_t { Network, Succeed, Fail };
/// Set how multicast joins behave; the test binary starts every case on Succeed.
void setTestJoin(TestJoin mode);
/// The group a socket left last, as a host test reads it, copied into `out`.
void testLastLeftGroup(uint8_t out[4]);
#endif
/// A neighbor's MAC from the address table this device's IPv4 stack keeps (ARP, RFC 826); false when the table holds none for `ip`, as before any exchange with it.
bool arpLookup(const uint8_t ip[4], uint8_t mac[6]);

/// WiFi co-processor status for a board whose radio lives on a separate chip, empty on a native radio.
const char* coprocessorWifi();
/// The co-processor line as the last coprocessorWifi() left it, a plain read for a control to bind to.
const char* coprocessorLine() MM_NONBLOCKING;
/// The ESP-Hosted version this firmware's host side is built with, which the radio chip's firmware must match on major.minor; empty on a native radio.
const char* coprocessorHostVersion();
/// Whether the radio chip has settled on firmware that does not match the host's, the update the Firmware card offers.
bool coprocessorOutdated() MM_NONBLOCKING;
/// The wired link's name on the Network card: Ethernet on a board, and on a host the host's own network, whatever the OS routes through, unless a cabled adapter carries the link.
const char* wiredLinkName() MM_NONBLOCKING;
/// Whether the WiFi radio can start now: always on a native radio, and on a board whose radio is a separate chip once the link to it is up, since starting it before waits on the chip.
bool wifiRadioReady() MM_NONBLOCKING;
#ifndef ESP_PLATFORM   // host-test seams, which no ESP32 code calls
/// Make the radio wait for its chip as a WiFi coproc board's does, refusing the station and the access point until it is ready, for a host test.
void setTestWifiRadioReady(bool ready);
#endif
/// Start the link to the radio chip on its own task where it is still unstarted, so its firmware can be read and updated over Ethernet; false where there is no such chip or the task cannot start.
bool coprocessorConnect();
/// Shut the WiFi co-processor's link down and free its pins, where the chip has one; a no-op elsewhere. The next coprocessorConnect brings it back.
void coprocessorShutdown();

/// This host's LAN IPv4 as a dotted string, or empty, valid until the calling thread's next call; on ESP32 the device IP belongs to NetworkModule.
const char* deviceIp();

// Network: ESP32 only, stubs on host.

/// Override the per-chip default pin and PHY map with a board's own, before ethInit.
void setEthConfig(const EthPinConfig& cfg);
/// Bring up the Ethernet driver.
bool ethInit();
/// Tear down a running driver so ethInit can re-init with new config; safe when no driver runs.
void ethStop();
/// PHY link detected, meaning a cable is plugged; a fast check.
bool ethLinkUp() MM_NONBLOCKING;
/// An IP is assigned, meaning DHCP completed.
bool ethConnected() MM_NONBLOCKING;
/// Current IP as raw octets, all-zero meaning none yet.
void ethGetIPv4(uint8_t out[4]) MM_NONBLOCKING;

/// Put one complete Ethernet frame on the wire, below IP, which is how panel receiver cards are addressed.
bool ethSendRaw(const uint8_t* frame, size_t len) MM_NONBLOCKING;

/// End of one wall frame: hand what ethSendRaw batched to the wire and start a new batch.
void ethFlushRaw() MM_NONBLOCKING;

/// Claim the Ethernet interface for direct L2 use, or release it; reference-counted.
void ethClaimRawL2(bool claim);

/// True while any driver holds a raw-L2 claim, which separates a broken link from a deliberately leaseless one.
bool ethRawL2Claimed() MM_NONBLOCKING;

/// Send failures since boot, split by cause, the two being different faults with different fixes.
void ethSendFailCounts(uint32_t& linkDown, uint32_t& ringFull) MM_NONBLOCKING;

/// Consecutive send failures since the last success, which tells back-pressure from a wedged path.
uint32_t ethSendFailStreak() MM_NONBLOCKING;

/// Restart the driver after transmit has wedged; blocks for up to ~4 seconds, so it is not MM_NONBLOCKING: @xref{ethernet-transmit-can-wedge}.
bool ethRestartTx();

/// Negotiated link speed in Mbit/s, 0 when no link or no driver.
uint16_t ethLinkSpeedMbps() MM_NONBLOCKING;

/// Bind raw sending to a host interface by name; null or empty returns to capture mode.
bool ethBindRawInterface(const char* ifName);

/// Enumerate the host NICs raw sending could bind, with entry 0 always "none (capture only)".
size_t rawInterfaces(const char* const** optionsOut);
/// The bind name behind row `i`, which differs from its label on Windows; null for row 0.
const char* rawInterfaceName(size_t i);
#ifndef ESP_PLATFORM   // host-test seams, which no ESP32 code calls
/// Replace the enumeration with a fixed list, so the control is pinnable on a machine with no real NICs.
void setTestRawInterfaces(const char* const* names, size_t count);

// The frames ethSendRaw captured, active whenever no raw interface is bound.

/// The panel format's largest frame; a longer one records truncated, its true length still reported.
constexpr size_t kEthTestFrameMax = 1512;
/// How many frames were captured.
size_t ethTestFrameCount();
/// One captured frame's true length, even where the recording was truncated.
size_t ethTestFrameLength(size_t i);
/// One captured frame's bytes.
const uint8_t* ethTestFrameData(size_t i);
/// Drop every captured frame.
void ethTestClearFrames();
/// Make the next sends fail, so a test can exercise the link-down path.
void setTestEthSendFails(bool fail);
/// Override the reported link speed, so a test can exercise the too-slow-link status.
void setTestEthLinkSpeed(uint16_t mbps);
/// Fake a cabled interface holding `ip`, null removing it, so a host test can drive the Ethernet cascade.
void setTestEthIPv4(const uint8_t* ip);
/// Whether the host's own network counts as the wired link, as it does when MoonLight runs on a host; off in a host test that drives the cascade on the fakes alone.
void setTestDeviceNetwork(bool counts);
/// Fake one address table entry, `ip` answering with `mac`, null `ip` removing it, so a host test can drive arpLookup.
void setTestArpEntry(const uint8_t* ip, const uint8_t* mac);
/// Make ethRestartTx fail, so a test can exercise the recovery-failed path.
void setTestEthRestartFails(bool fail);
/// How many times ethRestartTx has run, which pins the once-per-wedge bound.
uint32_t ethRestartCountForTest();
#endif

/// Join `ssid` as a station.
bool wifiStaInit(const char* ssid, const char* password);
/// Whether the station is associated and holds an IP.
bool wifiStaConnected() MM_NONBLOCKING;
/// Whether the station has joined its network, with an address or still awaiting one: WiFi's counterpart of ethLinkUp.
bool wifiStaAssociated() MM_NONBLOCKING;
/// The station's IP as raw octets, on ethGetIPv4's contract.
void wifiStaGetIPv4(uint8_t out[4]) MM_NONBLOCKING;
/// This device's IP from whichever interface is up, Ethernet first, all-zero before either is.
inline void localIPv4(uint8_t out[4]) {
    ethGetIPv4(out);
    if (!out[0] && !out[1] && !out[2] && !out[3]) wifiStaGetIPv4(out);
}
/// Tear the station down.
void wifiStaStop();

/// Station RSSI in dBm, a negative number; 0 when the station is not associated.
int wifiStaRssi();

/// One network a scan found.
struct WifiNetwork {
    char   ssid[33];   ///< empty for a network that hides its name
    int8_t rssi;       ///< signal strength in dBm
    bool   secured;    ///< whether it asks for a password
};
/// Start a scan and return at once, false where the radio cannot scan now.
bool wifiScanStart();
/// Copy the finished scan's networks into `out`, strongest first, or -1 while a scan runs or none ran.
int wifiScanResults(WifiNetwork* out, int max);
#ifndef ESP_PLATFORM   // host-test seams, which no ESP32 code calls
/// Make the next scan find these networks, so a host test drives a radio-less host; a count of -1 keeps it running forever.
void setTestWifiScan(const WifiNetwork* networks, int count);
/// Make the next scans refuse to start, as a radio still starting or joining does.
void setTestWifiScanRefused(bool refused);
/// The gateway, mask and DNS server netGetIPv4 reports beside the faked address, as a lease would carry them.
void setTestNetLease(const uint8_t gw[4], const uint8_t mask[4], const uint8_t dns[4]);
/// Make mdnsInit succeed, as on a device, so a host test sees when the local name is advertised.
void setTestMdnsAvailable(bool available);
/// How many times mdnsInit has started advertising since the seam was set.
uint32_t testMdnsInitCount();
/// Make mdnsInit fail with this reason, as a board out of memory does; null succeeds again.
void setTestMdnsFailure(const char* why);
#endif

/// Why the station's last join attempt failed, so the card can say "incorrect password".
enum class WifiFailure : uint8_t { None, WrongPassword, NotFound, Other };
/// The last failure since the station started, None while it is joining or joined.
WifiFailure wifiStaLastFailure();
#ifndef ESP_PLATFORM   // host-test seams, which no ESP32 code calls
/// Make the next join fail this way, for a host test.
void setTestWifiFailure(WifiFailure why);
#endif

/// The associated access point's BSSID, zeroed when the station is not associated.
void wifiStaBssid(uint8_t out[6]);
/// The WiFi channel in use, 0 when the station is not associated.
int  wifiStaChannel();

/// A client interface whose addressing NetworkModule sets.
enum class NetIface : uint8_t { Sta, Eth };
/// Pin a static IPv4 config onto a client interface, stopping its DHCP client; idempotent.
void netSetStaticIPv4(NetIface iface, const uint8_t ip[4], const uint8_t gw[4],
                      const uint8_t mask[4], const uint8_t dns[4]);
/// Return a client interface to DHCP, re-leasing live while the device keeps running.
void netSetDhcp(NetIface iface);
/// A client interface's addressing as it runs now, leased or static; all zero where it has none.
void netGetIPv4(NetIface iface, uint8_t ip[4], uint8_t gw[4], uint8_t mask[4], uint8_t dns[4]);
#ifndef ESP_PLATFORM   // host-test seams, which no ESP32 code calls
/// Make wifiStaInit succeed, so a host test can drive the station cascade a radio-less host otherwise skips.
void setTestWifiStaAvailable(bool available);
/// Make the station joined to its network while it waits for an address, for a host test.
void setTestWifiStaAssociated(bool associated);
/// The network the station was last asked to join, so a host test sees the order the cascade tries them in.
const char* testLastStaSsid();
/// Make the station joined at this address, or not joined with null, for a host test.
void setTestWifiStaIPv4(const uint8_t* ip);
/// How many static-addressing applies reached the platform for one interface.
uint32_t testNetStaticApplyCount(NetIface iface);
/// How many times netSetDhcp restored DHCP on `iface`, so a host test sees a join leave no static address behind.
uint32_t testNetDhcpCount(NetIface iface);
#endif

/// How the device's own access point appears: its name and address, and a WPA2 password or empty for open.
struct WifiApConfig {
    const char* name;       ///< the network's name
    const char* ip;         ///< its own address, which its DHCP server hands out as the gateway
    const char* password;   ///< WPA2 at 8 characters or more, open otherwise
};
/// Bring up the SoftAP, alongside the station when that is running.
bool wifiApInit(const WifiApConfig& cfg);
#ifndef ESP_PLATFORM   // host-test seams, which no ESP32 code calls
/// Make wifiApInit succeed, so a host test drives the access point a radio-less host otherwise skips.
void setTestWifiApAvailable(bool available);
/// Put devices on the access point, for a host test.
void setTestWifiApClients(uint32_t n);
/// The access point's last configuration, for a host test.
const WifiApConfig& testLastApConfig();
#endif
/// Whether the SoftAP is up.
bool wifiApConnected();
/// Tear the SoftAP down.
void wifiApStop();
/// Stations associated with our SoftAP, 0 when it is down or empty.
uint32_t wifiApClientCount();

/// True when a socket is safe to open: the stack is initialized and an interface holds an IP.
bool networkReady();

/// What the network stack holds on the heap, as its bring-ups took and its shutdowns gave back, less the stacks of the tasks they started, which the task snapshot counts: total bytes and the part in PSRAM, 0 where the platform cannot tell.
void networkHeap(size_t& total, size_t& psram);
#ifndef ESP_PLATFORM   // host-test seams, which no ESP32 code calls
/// Report this much network heap, as a board's measured bring-up does.
void setTestNetworkHeap(size_t total, size_t psram);
#endif

/// Current WiFi transmit power in dBm, 0 when WiFi is not initialized.
int wifiTxPower();

/// Cap the WiFi transmit power in quarter-dBm units, 8 to 84; 0 keeps the stack default.
bool wifiSetTxPower(int8_t quarterDbm);
#ifndef ESP_PLATFORM   // host-test seams, which no ESP32 code calls
/// The last cap a test radio took, in quarter dBm, 0 while none was pushed.
int8_t testLastTxPower();
#endif

/// Advertise this device over mDNS as `_http._tcp` and `_wled._tcp`, which is how the WLED app and Home Assistant find it.
bool mdnsInit(const char* deviceName);
/// Why the last mdnsInit failed, the step and its error such as `init ESP_ERR_NO_MEM`; empty after one succeeded.
const char* mdnsError();
/// Announce one more service on this board's name, such as `_apple-midi`/`_udp`, now and after every mdnsInit; a second call moves its port.
void mdnsAdvertise(const char* type, const char* proto, uint16_t port);
/// Stop announcing a service mdnsAdvertise added.
void mdnsWithdraw(const char* type, const char* proto);
/// The port a service is announced on, 0 when it is not: what a test reads back.
uint16_t mdnsAdvertisedPort(const char* type, const char* proto);
/// Stop advertising but keep the stack up, so a later mdnsInit needs no full re-init.
void mdnsStop();
/// Free the mDNS stack, at release.
void mdnsShutdown();
/// Resolve a host name to its IPv4 address, `.local` by mDNS and any other by DNS; it blocks, so only a background task calls it.
bool resolveName(const char* name, uint8_t ip[4]);
/// How far a non-blocking lookup has come.
enum class LookupStep : uint8_t { Pending, Resolved, Failed };
/// Start or continue resolving `name`, non-blocking, for a board with no worker task. One name at a time: a new name abandons the one in flight, and a start may allocate once.
LookupStep resolveNameStep(const char* name, uint8_t ip[4]);

/// Store the DHCP hostname the next bring-up advertises; call it before ethInit or wifiStaInit.
void setHostname(const char* name);

/// One cleartext HTTP request to a LAN host, answering the status code or 0 on failure.
int httpRequest(const char* method, const char* host, uint16_t port, const char* path,
                const char* reqBody, uint32_t timeoutMs, char* body, size_t bodyLen);

/// One request to a full `http://` or `https://` URL: the method, an optional JSON body, and the answer into `body` when there is room. The HTTP status, or 0 when it failed or did not fit; it blocks, so only a background task calls it.
int httpFetch(const char* method, const char* url, const char* reqBody, uint32_t timeoutMs,
              char* body, size_t bodyLen);

/// Whether this build can make an outbound HTTPS request at all.
bool httpsAvailable() MM_NONBLOCKING;

/// Internal RAM one TLS session holds while it lasts, on a board whose TLS buffers cannot go to PSRAM: two record buffers and the handshake.
inline constexpr size_t kTlsSessionBytes = 45 * 1024;

/// Whether a request to `url` may start now. HTTPS on a board whose TLS lives in internal RAM waits while that RAM is under the floor plus one session, which would starve WiFi and the web server.
bool tlsRoom(const char* url);
#ifndef ESP_PLATFORM   // host-test seams, which no ESP32 code calls

/// Answer `httpFetch` from a test instead of the network, by the same signature; null restores the network.
using HttpFetchFn = int (*)(const char* method, const char* url, const char* reqBody, char* body, size_t bodyLen);
void setTestHttpFetch(HttpFetchFn fn);
#endif

// Improv provisioning over UART0, sharing the channel with logging, which writes registers directly.

/// What Improv tells the client about this device; the task copies the strings, so pass static storage.
struct ImprovDeviceInfo {
    const char* name;            ///< device hostname, such as "MM-3A7F"
    const char* chipFamily;      ///< "ESP32", "ESP32-S3" and the rest
    const char* firmwareVersion; ///< the running version
};
/// Start provisioning: credentials land in the caller's buffers and set `ready`, which its tick clears.
bool improvProvisioningInit(const ImprovDeviceInfo& info,
                            char* ssidOut, size_t ssidOutLen,
                            char* passwordOut, size_t passwordOutLen,
                            std::atomic<bool>* ready,
                            char* statusBuf, size_t statusBufLen,
                            uint8_t* txPowerOut = nullptr,
                            std::atomic<bool>* txPowerReady = nullptr,
                            char* opOut = nullptr, size_t opOutLen = 0,
                            std::atomic<bool>* opReady = nullptr);
/// Drain the serial listener, called every tick: the work itself where the listener has no task of its own, a no-op where a task runs it.
void improvPoll() MM_NONBLOCKING;

/// One UDP socket: the wire every light protocol sends and receives on.
class UdpSocket {
public:
    /// An unopened socket.
    UdpSocket() = default;
    /// Close whatever is open.
    ~UdpSocket();

    /// Sockets are not copied; a file descriptor has one owner.
    UdpSocket(const UdpSocket&) = delete;
    /// Sockets are not copy-assigned, for the same reason.
    UdpSocket& operator=(const UdpSocket&) = delete;

    /// Open the socket, answering whether it came up.
    bool open();
    /// Listen on a port, on one local address or on any interface; false when it is taken. Listening flips the whole socket non-blocking, sends included.
    bool bind(uint16_t port, const uint8_t localIp[4] = nullptr);
    /// Listen on a discovery port on any interface beside the other listeners of that port, each hearing every broadcast and multicast datagram, as mDNS responders share theirs; a board runs one process, so there it is bind.
    bool bindShared(uint16_t port);
    /// The port the socket listens on, the one the OS picked after a bind to port 0; 0 when unbound.
    uint16_t localPort() const;
    /// Receive one datagram, non-blocking: bytes copied, or -1 when none is pending; a datagram longer than `maxLen` is truncated, and `srcIp` and `srcPort` answer who sent it.
    int recvFrom(uint8_t* buf, size_t maxLen, uint8_t srcIp[4] = nullptr, uint16_t* srcPort = nullptr);
    /// The most sockets one wait watches.
    static constexpr int kMaxWaitSockets = 16;
    /// Block until any of the first kMaxWaitSockets `sockets` has a datagram waiting or `timeoutMs` passes: which are readable, bit i for `sockets[i]`, 0 on a timeout, -1 where a platform cannot wait.
    static int waitReadable(UdpSocket* const* sockets, int count, uint32_t timeoutMs);
    /// Send once to an explicit address.
    bool sendToAddr(const uint8_t ip[4], uint16_t port, const uint8_t* data, size_t len);
    /// One IPv4 multicast group, its octets in order.
    struct MulticastGroup { uint8_t ip[4]; };
    /// Join this set of groups, leaving the earlier one, and keep it while silent; `groups` stays the caller's, read unchanged until close() or the next call. Answers how many joined, as a full table refuses the rest.
    uint8_t setMulticastGroups(const MulticastGroup* groups, uint8_t count) {
        for (uint8_t i = 0; i < count_; i++) membership(groups_[i].ip, false);   // the earlier set is left, not leaked
        groups_ = groups;
        count_ = groups ? count : 0;
        rejoinNext_ = cycleJoined_ = 0;
        uint8_t joined = 0;
        for (uint8_t i = 0; i < count_; i++) joined += join(i);
        joined_.store(joined, std::memory_order_relaxed);
        lastJoinMs_ = millis();
        return joined;
    }
    /// Join one group, the set of one, its address kept in the socket.
    bool joinMulticast(const uint8_t group[4]) {
        setMulticastGroups(nullptr, 0);   // leave the earlier set while own_ still holds the group it joined
        for (int i = 0; i < 4; i++) own_.ip[i] = group[i];
        return setMulticastGroups(&own_, 1) == 1;
    }
    /// How many groups the last pass over the set joined, so a caller can say when only unicast reaches the rest.
    uint8_t multicastJoined() const { return joined_.load(std::memory_order_relaxed); }
    /// How many joins the socket made since it opened, for the tests.
    uint32_t joinCountForTest() const { return joinCount_.load(std::memory_order_relaxed); }
    /// How long a socket in a group stays silent before it joins again.
    static constexpr uint32_t kRejoinMs = 3000;
    /// Close it, which the destructor also does.
    void close();

private:
    int fd_ = -1;
    const MulticastGroup* groups_ = nullptr;   // the caller's set, or own_
    MulticastGroup own_ = {};   // the one group joinMulticast keeps
    uint8_t count_ = 0;         // groups in the set
    std::atomic<uint8_t> joined_{0};   // how many the last pass joined, which another thread reads
    uint8_t rejoinNext_ = 0;    // the group the next rejoin takes, 0 between passes
    uint8_t cycleJoined_ = 0;   // how many the pass under way joined
    uint32_t lastHeardMs_ = 0;  // millis of the last datagram received
    uint32_t lastJoinMs_ = 0;   // when the last pass ended
    std::atomic<uint32_t> joinCount_{0};
    /// Join (`join`) or leave a group, the one OS call both take.
    bool membership(const uint8_t group[4], bool join);
    bool join(uint8_t i) { joinCount_++; return membership(groups_[i].ip, true); }
    /// Forget the set, which close() does once the membership went with the socket.
    void forgetGroups() { groups_ = nullptr; count_ = rejoinNext_ = cycleJoined_ = 0; joined_.store(0, std::memory_order_relaxed); }
    /// Note a datagram. Silence is the socket's, since a datagram does not name its group; one group lost among streaming ones relies on IGMP's query and report cycle.
    void heard() { if (count_) lastHeardMs_ = millis(); }
    /// Join again, one group per call, while the socket is silent, since a router can stop forwarding a group to a member it has not heard from; this also retries a failed join.
    void keepMembership() {
        if (!count_) return;
        const uint32_t now = millis();
        if (rejoinNext_ == 0 && (now - lastHeardMs_ < kRejoinMs || now - lastJoinMs_ < kRejoinMs)) return;
        membership(groups_[rejoinNext_].ip, false);   // a fresh join announces the membership again
        cycleJoined_ += join(rejoinNext_);
        if (++rejoinNext_ == count_) { joined_.store(cycleJoined_, std::memory_order_relaxed); cycleJoined_ = rejoinNext_ = 0; lastJoinMs_ = now; }
    }
};

/// One TCP connection, non-blocking so a client leaves the render loop running.
class TcpConnection {
public:
    /// An unconnected socket.
    TcpConnection() = default;
    /// Adopt an already-open descriptor, which is what accept hands over.
    explicit TcpConnection(int fd) : fd_(fd) {}
    /// Close whatever is open.
    ~TcpConnection();

    /// Connections are not copied; a file descriptor has one owner.
    TcpConnection(const TcpConnection&) = delete;
    /// Connections are not copy-assigned, for the same reason.
    TcpConnection& operator=(const TcpConnection&) = delete;

    /// Move the descriptor, leaving the source empty.
    TcpConnection(TcpConnection&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    /// Close what this holds, then take the other's descriptor.
    TcpConnection& operator=(TcpConnection&& other) noexcept {
        if (this != &other) { close(); fd_ = other.fd_; other.fd_ = -1; }
        return *this;
    }

    /// How an in-flight connect is going.
    enum class ConnectResult : uint8_t { Pending, Connected, Failed };
    /// Start a connect and return at once; false is an immediate DNS or socket failure. The caller polls across ticks, enforces its own timeout and gates on networkReady().
    bool connectStart(const char* host, uint16_t port);
    /// Check that connect, non-blocking; call it after connectStart.
    ConnectResult connectPoll();

    /// Whether this holds an open socket.
    bool valid() const { return fd_ >= 0; }
    /// Read, non-blocking: bytes copied, 0 when the peer closed, -1 when no data is pending.
    int read(uint8_t* buf, size_t maxLen);

    /// The connected peer's IPv4 address, which a second channel back to it is addressed by.
    bool peerIPv4(uint8_t out[4]) const;
    /// The address this side was reached at, which tells a request through the access point from one through the station.
    bool localIPv4(uint8_t out[4]) const;
    /// Write every byte, blocking until it is sent, which an HTTP response needs.
    bool write(const uint8_t* data, size_t len);
    /// Write what the socket accepts now: the count written, 0 when full, -1 on error; the caller advances its own offset and calls again, streaming across ticks.
    int writeSome(const uint8_t* data, size_t len);

    /// Close it, which the destructor also does.
    void close();

private:
    int fd_ = -1;
};

/// A listening TCP socket: what the web UI and the preview stream are served from.
class TcpServer {
public:
    /// A server that is closed until `open` starts it listening.
    TcpServer() = default;
    /// Stop listening.
    ~TcpServer();

    /// Servers are not copied; a listening socket has one owner.
    TcpServer(const TcpServer&) = delete;
    /// Servers are not copy-assigned, for the same reason.
    TcpServer& operator=(const TcpServer&) = delete;

    /// Listen on a port.
    bool open(uint16_t port);
    /// Take the next pending connection, non-blocking; an invalid one means none waited.
    TcpConnection accept();
    /// Stop listening.
    void close();

private:
    int fd_ = -1;
};

}  // namespace mm::platform
