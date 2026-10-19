#pragma once

#include "core/module/MoonModule.h"
#include "core/system/MqttPacket.h"
#include "core/system/SystemModule.h"

namespace mm { class ControlModule; }   // presets published as the HA effect list; .cpp includes it
#include "platform/platform.h"

#include <cstdint>

namespace mm {

/// Lets a home-automation hub switch the light on and off and set its brightness and color over MQTT.
/// @card MqttModule.png
///
/// A small MQTT 3.1.1 client of our own, off until a broker is set, wired by code as a child of Network. Every command goes through the shared control path, so MQTT adds only a transport.
///
/// @guide
///
/// ## Which route
///
/// Home Assistant finds the device over the WLED integration with no broker. Use MQTT for Homebridge, a broker-only network or another hub. [Home automation](https://moonmodules.org/MoonLight/reference/home-automation.html#the-mqtt-topics) lists the topics.
///
/// ## Getting it connected
///
/// The status line says why. Give the broker computer's LAN address, never 127.0.0.1, and let port 1883 through its firewall.
///
/// @technical
///
/// **The client is our own.** MQTT 3.1.1 is small and standard, so the wire format lives in a tested header and this module owns the socket lifecycle. The topic prefix derives from a stable hardware id, so a rename never repoints topics.
///
/// **Home Assistant discovery.** With the opt-in on, a retained config makes a discovery-aware hub create a wired entity. It defaults off because the compatibility shim already gives that hub a richer light. The same gate publishes an update entity.
///
/// **Lifecycle.** Everything runs on the slow tick, off the render path, since MQTT is slow control. It connects lazily, subscribes, keeps alive, and publishes on change and on connect; a dropped socket reconnects with a backoff.
class MqttModule : public MoonModule {
public:
    /// Adopt the system module, whose name and address the published topics carry.
    void setSystemModule(SystemModule* s) { systemModule_ = s; }
    /// Publish presets as the hub's effect list, look-only ones alone so nothing rewires hardware.
    void setControlModule(ControlModule* c) { controlModule_ = c; }

    /// Prime the status line before the first connect attempt.
    void setup() override;
    /// Free the discovery buffers, which are allocated only while announcing.
    void release() override;
    /// Declare the broker settings, the discovery opt-in and the status readout.
    void defineControls() override;
    /// Re-home the socket when the broker, port or credentials change.
    void onControlChanged(const char* controlName) override;
    /// Connect on enable, and disconnect cleanly on disable.
    void onEnabled(bool enabled) override;
    /// Drive the connection: connect, keep alive, drain inbound, and publish on change.
    void tick1s() MM_NONBLOCKING override;

    /// Feed inbound bytes as if from the broker, since a test has no live one.
    void feedForTest(const uint8_t* bytes, size_t len);

    /// Capture every outbound packet, so a test can assert what the module emits.
    void enableSendCaptureForTest(uint8_t* buf, size_t cap);
    /// How many bytes that capture holds.
    size_t sentCaptureLenForTest() const { return sendCaptureLen_; }

    /// The heap floor while discovery announces, so a test asserts against this not a literal.
    static constexpr size_t kDiscoveryDynamicBytes = 320 + 448;

private:
    /// Where the connection stands, advanced by the tick and never blocking it.
    enum class Conn : uint8_t { Idle, ConnectingTcp, Connecting, Connected };

    /// Begin a non-blocking TCP connect.
    void startConnect();
    /// Send the connect packet once TCP is up.
    void sendConnectPacket();
    /// Drain inbound, keep alive, and publish what changed.
    void serviceConnected();
    /// Send one packet whole, reporting whether it all went.
    bool sendPacket(const uint8_t* data, size_t len);
    /// Close the socket and return to idle with a status line.
    void resetConnection(const char* status);
    /// Feed one byte to the parser, routing a completed publish.
    void handleInboundByte(uint8_t byte);
    /// Route one inbound publish onto the control it addresses.
    void routePublish(const char* topic, const uint8_t* payload, size_t payloadLen);
    /// Publish the readable topics whose local values changed.
    void publishState(bool force);
    /// Publish the friendly name on its retained topic.
    void publishName();
    /// Re-publish the name after a rename while connected.
    void maybeRepublishName();
    /// Drive one control through the shared primitive.
    void setControlValue(const char* control, const char* valueJson);
    /// Point the status slot at our own buffer.
    void setStatusLine(const char* msg);

    /// Build the discovery topic the hub watches for a light.
    void buildDiscoveryTopic(char* out, size_t cap) const;
    /// Build the availability topic the last will publishes to.
    void buildStatusTopic(char* out, size_t cap) const;
    /// Announce the light, or retract it with an empty retained config.
    void publishDiscovery(bool announce);
    /// Subscribe to the hub's own command topic.
    void subscribeHaSet();

    /// Build the discovery topic for the update entity, a second discovery component in the light's shape behind the same gate.
    void buildUpdateDiscoveryTopic(char* out, size_t cap) const;
    /// Announce the update entity, or retract it.
    void publishUpdateDiscovery(bool announce);
    /// Publish the installed and available versions, retained.
    void publishUpdateState();
    /// Subscribe to the install command.
    void subscribeUpdateSet();
    /// Start an install against the matching release asset.
    void handleUpdateInstall(const char* payload, size_t payloadLen);

    SystemModule* systemModule_ = nullptr;
    ControlModule* controlModule_ = nullptr;
    uint32_t lastPresetsRev_ = 0;   ///< the preset revision last announced
    char lastLook_[32] = "";        ///< the look last published, part of the change gate

    /// The topic root, constexpr so the compiler can prove the buffer bounds a runtime pointer would not.
    static constexpr const char kPrefixRoot[] = "MoonLight";
    /// Derived from the root rather than counted from it, so a renamed root cannot silently truncate every topic.
    static constexpr size_t kPrefixLen = sizeof(kPrefixRoot) + 1 + 6;
    /// Write the topic prefix for this device.
    void topicPrefix(char* out, size_t cap) const;
    /// Write one full topic from its suffix.
    void buildTopic(char* out, size_t cap, const char* suffix) const;

    /// The broker's hostname or IP address.
    /// @guide A hostname such as `homeassistant.lan` is resolved through DNS. On a laptop broker use its LAN address.
    char     broker_[64]   = "";
    /// The broker's TCP port.
    uint16_t port_         = 1883;
    /// The broker login, optional.
    char     username_[48] = "";
    /// The broker password, stored obfuscated like the WiFi one.
    char     password_[48] = "";
    /// Announce a Home Assistant discovery light, opt-in.
    /// @guide Off by default: Home Assistant already finds the device over the WLED integration, and both would list it twice. Turn it on for broker-only or cross-subnet setups. The firmware update entity follows the same switch.
    bool     haDiscovery_  = false;
    /// Where the connection stands, from disabled and idle through connected, or the error.
    char     statusStr_[64] = "disabled";

    platform::TcpConnection conn_;
    MqttInboundParser parser_;
    Conn     state_ = Conn::Idle;         ///< where the connection stands
    uint32_t lastPingSent_   = 0;         ///< when the last keepalive went out
    uint32_t lastActivity_   = 0;         ///< the last byte either way, which paces the keepalive
    uint32_t lastConnectTry_ = 0;         ///< the backoff clock
    uint32_t connectStartedMs_ = 0;       ///< when the current attempt began
    uint32_t nameSig_        = 0;         ///< detects a rename while connected
    uint16_t nextPacketId_   = 1;         ///< the next id to hand out

    /// The last published state, so a publish only emits on a change.
    bool    lastOn_    = false;
    uint8_t lastBri_   = 0;
    uint8_t lastPalette_ = 0xFF;          ///< a sentinel forcing the first publish
    bool    havePublished_ = false;

    bool     lastConnectFailed_ = false;  ///< widens the backoff, a bad hostname costing a lookup

    /// Allocated only while discovery publishes, so a device that never enables it pays nothing.
    static constexpr size_t kDiscoveryPayloadBase = 320;
    static constexpr size_t kDiscoveryBufBase     = 448;
    static_assert(kDiscoveryDynamicBytes == kDiscoveryPayloadBase + kDiscoveryBufBase,
                  "public test constant must track the actual buffer sizes");
    /// Bytes the effect list needs for the current presets, or zero when there is nothing.
    size_t haEffectListBytes() const;
    /// Serialize the effect list, reporting the bytes written.
    size_t writeHaEffectList(char* out, size_t cap) const;
    size_t discoveryPayloadLen_ = 0;      ///< what the buffers were sized at, so a change resizes
    size_t discoveryBufLen_     = 0;      ///< the framed packet's size
    char*    discoveryPayload_ = nullptr; ///< the config JSON is built here
    uint8_t* discoveryBuf_     = nullptr; ///< the framed packet is built here
    /// Allocate both buffers, reporting false when there is no heap for them.
    bool ensureDiscoveryBuffers();
    /// Free both, which release and turning discovery off both do.
    void freeDiscoveryBuffers();

    /// Test-only send capture buffer, null in production.
    uint8_t* sendCapture_    = nullptr;
    size_t   sendCaptureCap_ = 0;
    size_t   sendCaptureLen_ = 0;

    static constexpr uint16_t kKeepaliveSec = 30;          ///< how often to ping
    static constexpr uint32_t kReconnectBackoffMs = 5000;  ///< after a clean disconnect
    /// Reconnect backoff after a failure, longer since a bad hostname re-runs a blocking lookup each try.
    static constexpr uint32_t kFailedBackoffMs    = 30000;
    static constexpr uint32_t kConnectTimeoutMs = 8000;    ///< the whole connect handshake
};

} // namespace mm
