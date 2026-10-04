#pragma once

#include "core/util/format.h"   // formatTo: nonblocking formatting into a fixed buffer
#include "core/module/MoonModule.h"
#include "core/system/IpSettings.h"
#include "core/system/FilesystemModule.h"
#include "core/util/JsonSink.h"
#include "core/util/JsonUtil.h"   // restoreList: the recursive reader for the persisted rows
#include "platform/platform.h"

#include <cstdint>
#include <cstring>

namespace mm {

/// The WiFi station: the networks the device knows, in the order it prefers them, and the radio's settings.
///
/// A Network child: the network module runs the cascade, and this one holds what joining WiFi needs.
/// @card WiFiModule.png
class WiFiModule : public MoonModule, public ListSource {
public:
    /// No networks known yet.
    WiFiModule() = default;
    /// Free the rows, the scan's networks and a pending join.
    ~WiFiModule() override {
        platform::free(rows_);
        platform::free(found_);
        platform::free(request_);
        platform::free(pendingPassword_);
        platform::free(handoff_);
    }
    /// Not copyable, since it owns the rows.
    WiFiModule(const WiFiModule&) = delete;
    /// Not assignable, for the same reason.
    WiFiModule& operator=(const WiFiModule&) = delete;

    /// Keep the station configured whatever the toggle says, or the device drops off the network.
    bool respectsEnabled() const MM_NONBLOCKING override { return false; }

    /// Declare the networks in range, the known networks, and the radio's settings.
    void defineControls() override {
        MoonModule::defineControls();
        // First, since the phone that asked for the join reads them before the access point closes.
        if (handoff_) {
            controls_.addReadOnly("address", handoff_->address, sizeof(handoff_->address));
            controls_.addReadOnly("localName", handoff_->localName, sizeof(handoff_->localName));
        }
        controls_.addButton("scan");
        controls_.addReadOnly("scanned", scannedStr_, sizeof(scannedStr_));
        controls_.addList("available", available_);
        controls_.addList("known", *this);
        // Meaningful only while associated, so hidden elsewhere rather than showing zero.
        controls_.addReadOnlyInt("rssi", rssi_, "dBm");
        controls_.setHidden(controls_.count() - 1, !connected_);
        // Hidden where the radio is off, and expert-only as a tuning readout.
        controls_.addReadOnlyInt("txPower", txPower_, "dBm");
        controls_.setHidden(controls_.count() - 1, !radioOn_);
        controls_.setAdvanced(controls_.count() - 1);
        // Zero lifts any prior cap rather than leaving it sticky.
        controls_.addControl("txPowerSetting", txPowerSetting_, 0, 21);
        controls_.setHidden(controls_.count() - 1, !radioOn_);
        controls_.setAdvanced(controls_.count() - 1);
    }

    // What the network module's cascade asks of the station.

    /// How many networks the device knows.
    uint8_t knownCount() const MM_NONBLOCKING { return count_; }
    /// A known network's row id, which stays with it when the order changes, or 0 past the end.
    uint32_t idAt(uint8_t i) const MM_NONBLOCKING { return i < count_ ? rows_[i].id : 0; }
    /// Where the known network with this row id now is, or -1 when it was forgotten.
    int indexOfId(uint32_t id) const MM_NONBLOCKING { return id ? indexOf(id) : -1; }
    /// A known network's name, in priority order, or empty past the end.
    const char* ssidAt(uint8_t i) const MM_NONBLOCKING { return i < count_ ? rows_[i].ssid : ""; }
    /// A known network's password, or empty for an open network.
    const char* passwordAt(uint8_t i) const MM_NONBLOCKING { return i < count_ ? rows_[i].password : ""; }
    /// The static address a known network pins, or null where it runs DHCP.
    const uint8_t* staticIpAt(uint8_t i) const MM_NONBLOCKING {
        return (i < count_ && rows_[i].ipSettings == ipsettings::kStatic) ? rows_[i].ip : nullptr;
    }

    /// Pin a known network's static address onto the station, or do nothing where it runs DHCP.
    void applyStatic(uint8_t i) const {
        if (!staticIpAt(i)) return;
        const Row& r = rows_[i];
        platform::netSetStaticIPv4(platform::NetIface::Sta, r.ip, r.gateway, r.subnet, r.dns);
    }

    /// A hash over a known network's IP settings, so an edit to the joined one re-applies live.
    uint32_t ipSigAt(uint8_t i) const {
        if (i >= count_) return Fnv1a{}.h;
        const Row& r = rows_[i];
        return ipsettings::sig(r.ipSettings, r.ip, r.gateway, r.subnet, r.dns);
    }

    /// Remember a network at the top of the list, or update the row that already has its name, and save.
    bool remember(const char* ssid, const char* password) {
        if (!ssid || !ssid[0]) return false;
        int i = indexOfSsid(ssid);
        if (i < 0) {
            if (!grow()) { setStatus(kListFull, Severity::Warning); return false; }
            i = count_ - 1;
            rows_[i] = Row{};
            rows_[i].id = nextId_++;
            mm::formatTo(rows_[i].ssid, sizeof(rows_[i].ssid), "%s", ssid);
        }
        mm::formatTo(rows_[i].password, sizeof(rows_[i].password), "%s", password ? password : "");
        moveTo(static_cast<uint8_t>(i), 0);
        markDirty();
        FilesystemModule::noteDirty();
        return true;
    }

    /// The transmit-power cap in dBm, 0 for none.
    int16_t txPowerSetting() const MM_NONBLOCKING { return txPowerSetting_; }
    /// Set and save the transmit-power cap.
    void setTxPowerSetting(uint8_t dBm) {
        if (dBm > 21) return;
        txPowerSetting_ = dBm;
        markDirty();
        FilesystemModule::noteDirty();
    }

    /// Show the radio's readings and which known network carries the device, by its row id, rebuilding the card only when what is visible changes.
    void showRadio(int8_t rssi, int8_t txPower, bool radioOn, bool connected, uint32_t joinedId = 0, const uint8_t* ip = nullptr) {
        rssi_ = rssi;
        txPower_ = txPower;
        joinedId_ = connected ? joinedId : 0;
        if (ip) std::memcpy(joinedIp_, ip, 4);
        if (radioOn == radioOn_ && connected == connected_) return;
        radioOn_ = radioOn;
        connected_ = connected;
        rebuildControls();
    }

    /// Adopt the device name, which the `.local` links are made of.
    void setDeviceName(const char* name) { deviceName_ = name; }

    /// Show the links a phone on the access point follows to the device on the network it just joined.
    void showHandoff(const uint8_t ip[4]) {
        if (!handoff_) handoff_ = static_cast<Handoff*>(platform::alloc(sizeof(Handoff)));
        if (!handoff_) return;   // the address is on the Network card as well
        char q[16];
        formatDottedQuad(q, ip);
        mm::formatTo(handoff_->address, sizeof(handoff_->address), "http://%s/", q);
        mm::formatTo(handoff_->localName, sizeof(handoff_->localName), "http://%s.local/", deviceName_ ? deviceName_ : "");
        // A phone's sign-in screen has no address bar, so the link is opened in the browser.
        setStatus("joined: open a link above in your browser");
        rebuildControls();
    }
    /// The access point closed, so the links go.
    void endHandoff() {
        if (!handoff_) return;
        platform::free(handoff_);
        handoff_ = nullptr;
        setStatus("");
        rebuildControls();
    }

    /// Scan once the device's own access point is up with nobody on it, so a phone joining it finds the networks already listed.
    void onAccessPointStarted() { scanRequested_ = true; }

    // Joining a network from the card, connect-first as a phone does: the device joins at once, and saves a scanned network only once it joined.

    /// A join the user asked for from the card, which the network module carries out.
    struct JoinRequest {
        char ssid[33];
        char password[64];
        uint32_t knownId;   ///< the known network asked for, by row id, or 0 for one picked from the scan
        uint32_t seq;       ///< which request this is, so a Connect pressed during a join replaces it rather than riding it
    };
    /// The join waiting to be carried out, or null.
    const JoinRequest* joinRequest() const MM_NONBLOCKING { return request_; }
    /// The join succeeded: a scanned network is remembered at the top; the joined network's row id.
    uint32_t joinSucceeded() {
        uint32_t id = 0;
        if (request_ && request_->knownId) id = request_->knownId;
        else if (request_ && remember(request_->ssid, request_->password)) id = rows_[0].id;
        else if (request_) { endRequest(kListFull); return 0; }   // joined, but not kept: the card says why
        endRequest(nullptr);
        return id;
    }
    /// The join failed, which the card says in words, and the network is not saved.
    void joinFailed(platform::WifiFailure why) {
        endRequest(why == platform::WifiFailure::WrongPassword ? "incorrect password"
                 : why == platform::WifiFailure::NotFound ? "network not found" : "could not join");
    }
    /// The join cannot happen now, for the reason given.
    void joinRefused(const char* why) { endRequest(why); }

    // ListSource: the known networks.

    /// How many rows the list shows.
    uint8_t listRowCount() const override { return count_; }

    /// A row as the collapsed list shows it: the name, open when it has no password, and a static address.
    void writeListRow(JsonSink& sink, uint8_t row) const override {
        if (row >= count_) { sink.append("{}"); return; }
        const Row& r = rows_[row];
        sink.appendf("{\"id\":%lu,\"ssid\":", static_cast<unsigned long>(r.id));
        sink.writeJsonString(r.ssid);
        if (r.id == joinedId_ && joinedId_) {
            char ip[16];
            formatDottedQuad(ip, joinedIp_);
            sink.appendf(",\"joined\":\"✓ %s\"", ip);
        }
        if (!r.password[0]) sink.append(",\"security\":\"open\"");
        if (r.ipSettings == ipsettings::kStatic) {
            char ip[16];
            formatDottedQuad(ip, r.ip);
            sink.appendf(",\"ipSettings\":\"static %s\"", ip);
        }
        sink.append("}");
    }

    /// Each saved row carries its network's password.
    bool listHoldsSecrets() const override { return true; }

    /// A row as the saved file keeps it: the name and password first, which is what MoonBase reads.
    void writeListRowSaved(JsonSink& sink, uint8_t row) const override {
        if (row >= count_) { sink.append("{}"); return; }
        const Row& r = rows_[row];
        sink.appendf("{\"id\":%lu,\"ssid\":", static_cast<unsigned long>(r.id));
        sink.writeJsonString(r.ssid);
        sink.append(",\"password\":");
        sink.writeJsonString(r.password);
        sink.appendf(",\"ipSettings\":%u", static_cast<unsigned>(r.ipSettings));
        const uint8_t* quads[4] = {r.ip, r.gateway, r.subnet, r.dns};
        for (int k = 0; k < 4; k++) {
            char q[16];
            formatDottedQuad(q, quads[k]);
            sink.appendf(",\"%s\":\"%s\"", ipsettings::kFields[k], q);
        }
        sink.append("}");
    }

    /// The IP settings' two choices, shared by every row.
    void writeListOptionSets(JsonSink& sink) const override {
        sink.appendf("\"ipSettings\":[\"%s\",\"%s\"]", ipsettings::kOptions[0], ipsettings::kOptions[1]);
    }

    /// A row's editable fields: its name, its password, and its IP settings, the static fields only under Static.
    void writeListRowDetail(JsonSink& sink, uint8_t row) const override {
        if (row >= count_) { sink.append("{}"); return; }
        const Row& r = rows_[row];
        sink.append("{\"fields\":[{\"name\":\"ssid\",\"type\":\"text\",\"value\":");
        sink.writeJsonString(r.ssid);
        sink.append("},{\"name\":\"password\",\"type\":\"password\",\"value\":");
        writeObfuscatedPassword(sink, r.password);   // as a Password control shows one
        sink.appendf("},{\"name\":\"ipSettings\",\"type\":\"select\",\"value\":%u,\"optionsRef\":\"ipSettings\"}",
                     static_cast<unsigned>(r.ipSettings));
        sink.append(",{\"name\":\"connect\",\"type\":\"button\",\"label\":\"Connect\"}");
        if (r.ipSettings == ipsettings::kStatic) {
            const uint8_t* quads[4] = {r.ip, r.gateway, r.subnet, r.dns};
                for (int k = 0; k < 4; k++) {
                char q[16];
                formatDottedQuad(q, quads[k]);
                sink.appendf(",{\"name\":\"%s\",\"type\":\"ipv4\",\"value\":\"%s\"}", ipsettings::kFields[k], q);
            }
        }
        sink.append("]}");
    }

    /// The rows are edited from the card.
    bool isEditableList() const override { return true; }

    /// Add an empty network for the user to name.
    bool addListRow(uint32_t& outId) override {
        if (!grow()) return false;
        Row& r = rows_[count_ - 1];
        r = Row{};
        r.id = nextId_++;
        outId = r.id;
        return true;
    }

    /// Forget a network.
    bool deleteListRow(uint32_t id) override {
        const int i = indexOf(id);
        if (i < 0) return false;
        for (int j = i; j + 1 < count_; j++) rows_[j] = rows_[j + 1];
        count_--;
        return true;
    }

    /// Move a network up or down the priority order.
    bool moveListRow(uint32_t id, uint8_t to) override {
        const int i = indexOf(id);
        if (i < 0) return false;
        moveTo(static_cast<uint8_t>(i), to);
        return true;
    }

    /// Edit a network's name or password, or connect to it now.
    bool setListRowField(uint32_t id, const char* field, const char* valueJson) override {
        const int i = indexOf(id);
        if (i < 0) return false;
        Row& r = rows_[i];
        if (std::strcmp(field, "connect") == 0) return requestJoin(r.ssid, r.password, r.id);
        if (std::strcmp(field, "ssid") == 0) {
            mm::json::parseString(valueJson, "value", r.ssid, sizeof(r.ssid));
            return true;
        }
        if (std::strcmp(field, "password") == 0) {
            mm::json::parseString(valueJson, "value", r.password, sizeof(r.password));
            return true;
        }
        if (std::strcmp(field, "ipSettings") == 0) {
            const int v = mm::json::parseInt(valueJson, "value");
            if (v != ipsettings::kDhcp && v != ipsettings::kStatic) return false;
            r.ipSettings = static_cast<uint8_t>(v);
            return true;
        }
        // An address that does not parse is refused rather than stored as zero.
        uint8_t* quad = std::strcmp(field, "ip") == 0 ? r.ip
                      : std::strcmp(field, "gateway") == 0 ? r.gateway
                      : std::strcmp(field, "subnet") == 0 ? r.subnet
                      : std::strcmp(field, "dns") == 0 ? r.dns : nullptr;
        if (!quad) return false;
        char text[16] = {};
        mm::json::parseString(valueJson, "value", text, sizeof(text));
        return parseDottedQuad(text, quad);
    }

    /// The rows are configuration, restored by restoreList.
    bool persistsList() const override { return true; }

    /// Restore the known networks from the saved file.
    bool restoreList(const char* json, const char* key) override {
        mm::json::JsonDoc doc;
        if (!mm::json::parse(json, doc)) return false;
        const mm::json::JsonNode* arr = mm::json::member(doc, doc.rootNode(), key);
        if (!arr || arr->type != mm::json::JsonType::Array) return false;
        count_ = 0;
        const int n = mm::json::arraySize(doc, arr);
        for (int k = 0; k < n && k < kMaxKnown; k++) {
            if (!grow()) break;
            const mm::json::JsonNode* el = mm::json::element(doc, arr, k);
            Row& r = rows_[count_ - 1];
            r = Row{};
            r.id = static_cast<uint32_t>(mm::json::readInt(mm::json::member(doc, el, "id"), 0));
            mm::json::readString(mm::json::member(doc, el, "ssid"), r.ssid, sizeof(r.ssid));
            mm::json::readString(mm::json::member(doc, el, "password"), r.password, sizeof(r.password));
            r.ipSettings = mm::json::readInt(mm::json::member(doc, el, "ipSettings"), ipsettings::kDhcp) == ipsettings::kStatic ? ipsettings::kStatic : ipsettings::kDhcp;
            uint8_t* quads[4] = {r.ip, r.gateway, r.subnet, r.dns};
                for (int q = 0; q < 4; q++) {
                char text[16] = {};
                mm::json::readString(mm::json::member(doc, el, ipsettings::kFields[q]), text, sizeof(text));
                if (text[0]) parseDottedQuad(text, quads[q]);
            }
            if (r.id == 0) r.id = nextId_;
            if (r.id >= nextId_) nextId_ = r.id + 1;   // never reissue a saved id
        }
        return true;
    }

    /// Run a requested scan and keep its age on the card, without blocking: the scan's results arrive on a later tick.
    void tick1s() MM_NONBLOCKING override {
        MoonModule::tick1s();
        const uint32_t now = platform::millis();
        if (scanRequested_ && !scanning_) {
            scanRequested_ = false;
            if (platform::wifiScanStart()) {
                scanning_ = true;
                scanStartedMs_ = now;
                // A scan takes the radio off-channel for a few seconds, which drops incoming lights while connected.
                setStatus(connected_ ? "scanning, output may stutter for a few seconds" : "scanning");
            } else {
                setStatus("cannot scan now: the radio is off", Severity::Warning);
            }
        }
        if (scanning_) collectScan(now);
        // A scan the radio dropped, or one whose results went elsewhere, ends after its time rather than reading "scanning" for good.
        if (scanning_ && now - scanStartedMs_ > kScanTimeoutMs) {
            scanning_ = false;
            setStatus("the scan did not finish: press scan to try again", Severity::Warning);
        }
        // The age reads "scanned 3 min ago", since the device does not scan on its own while connected.
        if (scannedMs_) {
            const uint32_t min = (now - scannedMs_) / 60000u;
            if (min == 0) mm::formatTo(scannedStr_, sizeof(scannedStr_), "just now");
            else mm::formatTo(scannedStr_, sizeof(scannedStr_), "%u min ago", static_cast<unsigned>(min));
        } else {
            mm::formatTo(scannedStr_, sizeof(scannedStr_), "not yet: press scan");
        }
    }

    /// Start a scan when `scan` is pressed.
    void onControlChanged(const char* name) override {
        if (std::strcmp(name, "scan") == 0) scanRequested_ = true;
    }

private:
    /// What the card says when a network cannot be remembered.
    static constexpr const char* kListFull = "known networks full: forget one to add another";

    /// One known network: the name a router broadcasts, its passphrase as WPA2 bounds them, and its own IP settings.
    struct Row {
        uint32_t id = 0;
        char ssid[33] = {};
        char password[64] = {};
        uint8_t ipSettings = ipsettings::kDhcp;   ///< DHCP or Static, per network as phones keep it
        uint8_t ip[4] = {};
        uint8_t gateway[4] = {};
        uint8_t subnet[4] = {255, 255, 255, 0};
        uint8_t dns[4] = {};
    };
    /// Enough for a workshop, a venue and a few more, kept small since each row is RAM.
    static constexpr uint8_t kMaxKnown = 8;

    Row*     rows_ = nullptr;   ///< the known networks in priority order, on the heap and sized to the count, so a device knowing one pays for one
    uint8_t  count_ = 0;
    uint32_t nextId_ = 1;

    // The scan: its networks on the heap only while there are some, and the join the card asked for.
    static constexpr uint8_t kMaxFound = 16;
    platform::WifiNetwork* found_ = nullptr;
    uint8_t  foundCount_ = 0;
    uint32_t scannedMs_ = 0;      ///< when the last scan finished, 0 before the first
    bool     scanRequested_ = false;
    bool     scanning_ = false;
    uint32_t scanStartedMs_ = 0;
    static constexpr uint32_t kScanTimeoutMs = 15000;   ///< a scan takes two to five seconds
    char     scannedStr_[24] = {};
    JoinRequest* request_ = nullptr;   ///< on the heap only while a join waits
    uint32_t requestSeq_ = 0;          ///< counts the requests, so the network module tells a replaced one from the one it started
    char*    pendingPassword_ = nullptr;   ///< what was typed for a scanned network, on the heap only while typed
    uint32_t pendingId_ = 0;           ///< the scanned row that password belongs to
    uint32_t joinedId_ = 0;            ///< the known network carrying the device, by row id, so a reorder keeps it; 0 for none
    uint8_t  joinedIp_[4] = {};
    /// The links to the device on its new network, on the heap only while the access point is held after a join from it.
    struct Handoff {
        char address[24];     ///< http://255.255.255.255/
        char localName[40];   ///< http://, a 23-character device name, .local/
    };
    Handoff* handoff_ = nullptr;
    const char* deviceName_ = nullptr;   ///< the system module's, which outlives this one

    /// Collect a finished scan straight into the heap, keeping named networks once each at their strongest, which the driver lists first.
    void collectScan(uint32_t now) {
        auto* buf = static_cast<platform::WifiNetwork*>(platform::alloc(sizeof(platform::WifiNetwork) * kMaxFound));
        if (!buf) return;   // retried on the next tick
        const int n = platform::wifiScanResults(buf, kMaxFound);
        if (n < 0) { platform::free(buf); return; }   // still scanning
        scanning_ = false;
        scannedMs_ = now == 0 ? 1 : now;
        uint8_t kept = 0;
        for (int i = 0; i < n; i++) {
            if (!buf[i].ssid[0]) continue;   // a hidden network, joined by typing its name into a known row
            bool dup = false;
            for (uint8_t k = 0; k < kept; k++) if (std::strcmp(buf[k].ssid, buf[i].ssid) == 0) { dup = true; break; }
            if (!dup) buf[kept++] = buf[i];
        }
        platform::free(found_);
        found_ = kept ? buf : nullptr;
        if (!kept) platform::free(buf);
        foundCount_ = kept;
        platform::free(pendingPassword_);   // a new scan renumbers the rows
        pendingPassword_ = nullptr;
        pendingId_ = 0;
        setStatus("");
    }

    /// Queue a join for the network module, replacing any waiting one.
    bool requestJoin(const char* ssid, const char* password, uint32_t knownId) {
        if (!ssid || !ssid[0]) return false;
        if (!request_) request_ = static_cast<JoinRequest*>(platform::alloc(sizeof(JoinRequest)));
        if (!request_) return false;
        mm::formatTo(request_->ssid, sizeof(request_->ssid), "%s", ssid);
        mm::formatTo(request_->password, sizeof(request_->password), "%s", password ? password : "");
        request_->knownId = knownId;
        request_->seq = ++requestSeq_;
        mm::formatTo(statusStr_, sizeof(statusStr_), "joining %s", ssid);
        setStatus(statusStr_);
        return true;
    }

    /// End a join, saying why when it did not happen; what was typed stays for a retry, and goes once the network is known.
    void endRequest(const char* why) {
        platform::free(request_);
        request_ = nullptr;
        if (why) { setStatus(why, Severity::Warning); return; }
        platform::free(pendingPassword_);
        pendingPassword_ = nullptr;
        pendingId_ = 0;
        setStatus("");
    }
    char statusStr_[48] = {};

    /// The scan's networks as a list: tapping one shows its password field and Connect.
    struct Available : ListSource {
        WiFiModule& w;
        explicit Available(WiFiModule& owner) : w(owner) {}
        uint8_t listRowCount() const override { return w.foundCount_; }
        bool isEditableList() const override { return true; }
        bool listRowsFixed() const override { return true; }
        /// Signal as bars and a lock on secured networks, as every WiFi picker shows them.
        void writeListRow(JsonSink& sink, uint8_t row) const override {
            if (row >= w.foundCount_) { sink.append("{}"); return; }
            const platform::WifiNetwork& n = w.found_[row];
            const int r = n.rssi;
            const char* bars = r >= -55 ? "▂▄▆█" : r >= -67 ? "▂▄▆_" : r >= -78 ? "▂▄__" : "▂___";
            sink.appendf("{\"id\":%u,\"ssid\":", static_cast<unsigned>(row + 1));
            sink.writeJsonString(n.ssid);
            sink.appendf(",\"signal\":\"%s\"", bars);
            if (n.secured) sink.append(",\"security\":\"🔒\"");
            const int k = w.indexOfSsid(n.ssid);
            if (k >= 0) sink.append(w.rows_[k].id == w.joinedId_ ? ",\"known\":\"✓ connected\"" : ",\"known\":\"known\"");
            sink.append("}");
        }
        /// A password field where the network asks for one and is not known yet, then Connect.
        void writeListRowDetail(JsonSink& sink, uint8_t row) const override {
            if (row >= w.foundCount_) { sink.append("{}"); return; }
            const platform::WifiNetwork& n = w.found_[row];
            sink.append("{\"fields\":[");
            if (n.secured && w.indexOfSsid(n.ssid) < 0) {
                sink.append("{\"name\":\"password\",\"type\":\"password\",\"value\":");
                const bool mine = w.pendingPassword_ && w.pendingId_ == row + 1u;
                writeObfuscatedPassword(sink, mine ? w.pendingPassword_ : "");
                sink.append("},");
            }
            sink.append("{\"name\":\"connect\",\"type\":\"button\",\"label\":\"Connect\"}");
            // Read before Connect, since a phone often leaves the access point during the join and lands back on its own network, where this link reaches the device.
            if (w.deviceName_ && w.deviceName_[0])
                sink.appendf(",{\"name\":\"afterJoining\",\"readonly\":true,\"value\":\"http://%s.local/\"}", w.deviceName_);
            sink.append("]}");
        }
        bool setListRowField(uint32_t id, const char* field, const char* valueJson) override {
            if (id == 0 || id > w.foundCount_) return false;
            const platform::WifiNetwork& n = w.found_[id - 1];
            if (std::strcmp(field, "password") == 0) {
                if (!w.pendingPassword_) w.pendingPassword_ = static_cast<char*>(platform::alloc(64));
                if (!w.pendingPassword_) return false;
                w.pendingPassword_[0] = '\0';
                mm::json::parseString(valueJson, "value", w.pendingPassword_, 64);
                w.pendingId_ = id;
                return true;
            }
            if (std::strcmp(field, "connect") != 0) return false;
            const int k = w.indexOfSsid(n.ssid);
            if (k >= 0) return w.requestJoin(n.ssid, w.rows_[k].password, w.rows_[k].id);
            const bool mine = w.pendingPassword_ && w.pendingId_ == id;
            return w.requestJoin(n.ssid, mine ? w.pendingPassword_ : "", 0);
        }
    };
    Available available_{*this};

    int8_t  rssi_ = 0;          ///< the signal strength, stored directly rather than formatted
    int8_t  txPower_ = 0;       ///< the radio's current power
    int16_t txPowerSetting_ = 0;
    bool    radioOn_ = false;   ///< whether the radio is up, which shows the power readings
    bool    connected_ = false; ///< whether the station is associated, which shows the signal

    /// Make room for one more row, reallocating to the exact size since rows change rarely.
    bool grow() {
        if (count_ >= kMaxKnown) return false;
        auto* bigger = static_cast<Row*>(platform::alloc(sizeof(Row) * (count_ + 1u)));
        if (!bigger) return false;
        if (count_) std::memcpy(bigger, rows_, sizeof(Row) * count_);
        platform::free(rows_);
        rows_ = bigger;
        count_++;
        return true;
    }

    /// Move one row to a new position, shifting the ones between.
    void moveTo(uint8_t from, uint8_t to) {
        if (from >= count_) return;
        if (to >= count_) to = static_cast<uint8_t>(count_ - 1);
        const Row moved = rows_[from];
        if (to > from) for (int j = from; j < to; j++) rows_[j] = rows_[j + 1];
        else           for (int j = from; j > to; j--) rows_[j] = rows_[j - 1];
        rows_[to] = moved;
    }

    int indexOf(uint32_t id) const {
        for (uint8_t i = 0; i < count_; i++) if (rows_[i].id == id) return i;
        return -1;
    }
    int indexOfSsid(const char* ssid) const {
        for (uint8_t i = 0; i < count_; i++) if (std::strcmp(rows_[i].ssid, ssid) == 0) return i;
        return -1;
    }
};

} // namespace mm
