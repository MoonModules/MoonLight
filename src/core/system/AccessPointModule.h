#pragma once

#include "core/util/DeviceName.h"   // defaultDeviceName: the anonymous name it broadcasts
#include "core/module/MoonModule.h"
#include "core/util/CaptivePortal.h"
#include "core/util/fnv.h"
#include "platform/platform.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mm {

/// The device's own WiFi network: when it opens, and the phones on it.
///
/// It is named `MM-` and four MAC digits rather than after the device. The first known network's password protects it.
///
/// Switched on four times in a row, each within a few seconds, it opens for that boot without a password, whatever `opens` says. That is the way back to a device whose router changed, with every setting kept.
///
/// A Network child: the network module opens and closes it, and this one holds how it appears and answers its DNS.
/// @card AccessPointModule.png
class AccessPointModule : public MoonModule {
public:
    /// When the access point opens, in WLED's words.
    enum class Opens : uint8_t { OnFailure, Always, FirstSetup };
    /// The switch-ons in a row that open it for one boot.
    static constexpr uint8_t kOpeningPowerOns = 4;

    /// Not answering DNS yet.
    AccessPointModule() = default;
    /// Close the DNS socket and free its buffer.
    ~AccessPointModule() override { stopped(); }
    /// Not copyable, since it owns the DNS buffer.
    AccessPointModule(const AccessPointModule&) = delete;
    /// Not assignable, for the same reason.
    AccessPointModule& operator=(const AccessPointModule&) = delete;

    /// Keep the access point configured whatever the toggle says, or a device that loses WiFi is unreachable.
    bool respectsEnabled() const MM_NONBLOCKING override { return false; }

    /// Read once whether switching opened it, and say so, since it then has no password.
    void setup() override {
        switched_ = platform::bootRecord().quickPowerOns >= kOpeningPowerOns;
        if (switched_) {
            std::printf("AccessPointModule: switched on %u times in a row, open without a password for this boot\n", static_cast<unsigned>(kOpeningPowerOns));
            setStatus("Open for this boot without a password: switched on and off in quick succession", Severity::Warning);
        }
        MoonModule::setup();
    }


    /// Declare when it opens, how it appears, and who is on it.
    void defineControls() override {
        MoonModule::defineControls();
        controls_.addSelect("opens", opens_, kOpensOptions, 3);
        name();   // filled before it is bound
        controls_.addReadOnly("name", name_, sizeof(name_));
        // Meaningful only while it runs, so hidden elsewhere rather than showing zero.
        controls_.addReadOnlyInt("clients", clients_, "");
        controls_.setHidden(controls_.count() - 1, !running_);
    }

    // What the network module's cascade asks of the access point.

    /// When it opens: always for a boot opened by switching; first setup only acts as on failure until a network or Ethernet is configured, and keeps it closed after.
    Opens opens(bool othersConfigured) const MM_NONBLOCKING {
        if (switched_) return Opens::Always;
        const auto o = static_cast<Opens>(opens_ < 3 ? opens_ : 0);
        return (o == Opens::FirstSetup && !othersConfigured) ? Opens::OnFailure : o;
    }

    /// The password it carries: the first known network's, which the owner knows and a stranger does not, or empty when that is no WPA2 passphrase.
    static const char* passwordFor(const char* firstKnownNetwork) {
        const size_t n = firstKnownNetwork ? std::strlen(firstKnownNetwork) : 0;
        return n >= 8 && n <= 63 ? firstKnownNetwork : "";
    }

    // Anonymous, as such networks usually are, so the air does not say whose device it is; the device's own name stays on its home network.
    /// The name it broadcasts: `MM-` and the last four digits of the MAC, the same for the app and MoonBase.
    const char* name() const {
        if (!name_[0]) {
            uint8_t mac[6];
            platform::getMacAddress(mac);
            defaultDeviceName(mac, name_, sizeof(name_));
        }
        return name_;
    }

    /// How it appears: its name at the captive portal's address, with the first known network's password.
    platform::WifiApConfig config(const char* firstKnownNetwork) const {
        return platform::WifiApConfig{name(), captive::kAddressText, password(firstKnownNetwork)};
    }

    /// A hash over its password, so a change to the first known network's re-opens it live.
    uint32_t sig(const char* firstKnownNetwork) const MM_NONBLOCKING {
        const char* pw = password(firstKnownNetwork);
        Fnv1a f;
        f.add(pw, std::strlen(pw));
        return f.h;
    }

    /// It opened: answer every name with its own address, so a joining phone shows the UI.
    void started() {
        if (!running_) { running_ = true; rebuildControls(); }
        if (dnsBuf_) return;
        dnsBuf_ = static_cast<uint8_t*>(platform::alloc(captive::kMaxMessage));
        // Bound to its own address, answering only the phones on it; a failure loses only the sign-in screen, the UI staying reachable by its address.
        if (!dnsBuf_ || !dns_.open() || !dns_.bind(53, captive::kAddress)) { dns_.close(); platform::free(dnsBuf_); dnsBuf_ = nullptr; }
    }
    /// It closed: release the DNS socket and its buffer.
    void stopped() {
        dns_.close();
        platform::free(dnsBuf_);
        dnsBuf_ = nullptr;
        clients_ = 0;
        if (running_) { running_ = false; rebuildControls(); }
    }
    /// Show how many devices are on it.
    void showClients(uint32_t n) { clients_ = static_cast<int8_t>(n > 127 ? 127 : n); }
    /// Whether the DNS responder is answering, for a host test.
    bool answeringDns() const { return dnsBuf_ != nullptr; }

    /// Answer the DNS queries waiting, a few per tick so a flood cannot stall the frame.
    void tick20ms() MM_NONBLOCKING override {
        MoonModule::tick20ms();
        if (!dnsBuf_) return;
        for (int i = 0; i < 4; i++) {
            uint8_t from[4];
            uint16_t port = 0;
            const int n = dns_.recvFrom(dnsBuf_, captive::kMaxMessage, from, &port);
            if (n <= 0) break;
            const size_t len = captive::dnsReply(dnsBuf_, static_cast<size_t>(n), captive::kMaxMessage, captive::kAddress);
            if (len) dns_.sendToAddr(from, port, dnsBuf_, len);
        }
    }


private:
    static constexpr const char* kOpensOptions[] = {"on failure", "always", "first setup only"};

    // None after switching, since switching proves a hand on the device, and the owner of a changed router may no longer know the old password.
    /// The password this boot carries.
    const char* password(const char* firstKnownNetwork) const { return switched_ ? "" : passwordFor(firstKnownNetwork); }

    uint8_t opens_ = 0;
    bool    switched_ = false;   ///< this boot opened it by switching on and off
    int8_t  clients_ = 0;
    mutable char name_[8] = {};   ///< filled on first use from the MAC
    bool    running_ = false;

    platform::UdpSocket dns_;
    uint8_t* dnsBuf_ = nullptr;   ///< on the heap only while the access point runs
};

}  // namespace mm
