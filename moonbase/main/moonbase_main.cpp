/// MoonBase: the second boot image.
///
/// A 4 MB board has room for one application, not two.
/// Its partition table carries this small image in the `factory` slot instead of a second copy of the firmware.
/// When the application must be replaced, the device reboots here.
/// MoonBase owns the board, writes the new firmware into the application slot it is not itself running from, and hands control back.
///
/// @moreinfo
///
/// ## Why it shares no code with the application
///
/// Everything here is written directly against ESP-IDF.
/// It shares no code with the application on purpose: the app's platform layer pulls in RMT, I2S, PSRAM and the JIT, which measured 788 KB with an empty entry point.
/// This file plus its sdkconfig measures around a quarter of the flash instead.
/// The other half of the budget is in ../sdkconfig.defaults, which is part of the design.
///
/// ## The boot flow
///
/// In order:
///   1. mount the application's filesystem read-only and read the stored WiFi credentials
///   2. bring up the network: Ethernet if the board has it, else WiFi STA, else our own AP
///   3. serve one page: install by upload, or install from a URL
///   4. write the application slot, point the bootloader at it, reboot

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "core/util/FirmwareImage.h"  // identify(): shared with the app, see main/CMakeLists.txt
#include "core/util/ConfigScrape.h"   // the keys this image reads out of the app's config, shared so the app's test runs the same scraper
#include "core/util/CaptivePortal.h"  // the access point's address, DNS answer and redirect rule, shared with the app's portal
#include "esp_app_desc.h"    // esp_app_get_description: this image's own version
#include "esp_https_ota.h"
#include "esp_littlefs.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "soc/gpio_num.h"
#include "soc/soc_caps.h"   // SOC_EMAC_SUPPORTED: the S3 and other WiFi-only parts have no EMAC
#include "esp_eth.h"
#if SOC_EMAC_SUPPORTED
#include "esp_eth_mac_esp.h"   // esp_eth_mac_new_esp32: only exists on a chip with an EMAC
#endif
#include "esp_eth_netif_glue.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "nvs_flash.h"

namespace {

// The app's config volume, labeled `littlefs` or, on older tables, `spiffs`; only ever read, so a failed install cannot corrupt user config.
struct FsCandidate { esp_partition_subtype_t subtype; const char* label; };
constexpr FsCandidate kFsCandidates[] = {
    {ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, "littlefs"},
    {ESP_PARTITION_SUBTYPE_DATA_SPIFFS,   "spiffs"},
};
constexpr const char* kFsMountPoint     = "/fs";
constexpr const char* kNetworkConfig    = "/fs/.config/NetworkModule.json";
// The app persists its build variant here, the one fact this chip-specific image cannot know about its board.
constexpr const char* kSystemConfig     = "/fs/.config/SystemModule.json";

constexpr const char* kApName    = "MoonBase";   // for a device that never saved a name

constexpr int kHttpPort = 80;

// The app's known networks in its priority order, tried as the app tries them.
constexpr uint8_t kMaxNetworks = 8;
struct Network { char ssid[33]; char password[65]; mm::configscrape::SavedIp ip; };
Network networks_[kMaxNetworks] = {};
uint8_t networkCount_ = 0;
// The app's own access point, named after the device and protected as the user set it, so a phone on it stays on through the restart into this image.
char apName_[33] = {};
char apPassword_[64] = {};
char status_[96] = "idle";

EventGroupHandle_t netEvents_;
constexpr int kNetGotIp = BIT0;
constexpr int kNetStaDown = BIT1;   // the station dropped its attempt
// Set while the station moves to the next known network, so the attempt being dropped is not retried under the old config.
volatile bool switchingNetwork_ = false;

// ---------------------------------------------------------------------------------------------
// Credentials
// ---------------------------------------------------------------------------------------------

// The board's Ethernet wiring from the app's config: type 0 or absent is none, and an absent pin keeps the silicon default, as in the app.
struct {
    int  type       = 0;
    int  phyAddr    = -1;      // -1: scan the MDIO bus
    int  rstGpio    = -1;
    int  mdcGpio    = -1;      // <0: leave the EMAC default
    int  mdioGpio   = -1;
    int  clockGpio  = 0;
    bool clockExtIn = true;
} ethCfg_;

// The Ethernet card's addressing, so a board the app pins to a static address answers there in recovery too.
mm::configscrape::SavedIp ethIp_;

// The board's WiFi TX cap in dBm, 0 for none: a board that browns out at full power must not do so in recovery.
int txPowerDbm_ = 0;

// The app's build variant ("esp32s3-zero"), or empty before the app has run, when the page offers every firmware for the chip.
char g_appVariant[24] = {};

// A config file whole, on the heap (caller frees), since a key sits wherever the app's module order and list lengths put it.
char* readConfig(const char* path) {
    constexpr long kMaxConfig = 32 * 1024;   // far past any real file; a corrupt size must not ask for the heap
    FILE* f = std::fopen(path, "r");
    if (!f) return nullptr;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    char* buf = (size > 0 && size <= kMaxConfig) ? static_cast<char*>(std::malloc(static_cast<size_t>(size) + 1)) : nullptr;
    if (buf) buf[std::fread(buf, 1, static_cast<size_t>(size), f)] = '\0';
    std::fclose(f);
    return buf;
}

void loadIdentity() {
    char* buf = readConfig(kSystemConfig);
    if (!buf) return;
    mm::configscrape::findString(buf, "firmware", g_appVariant, sizeof(g_appVariant));
    mm::configscrape::findString(buf, "deviceName", apName_, sizeof(apName_));
    std::free(buf);
}

// Read the stored WiFi credentials and Ethernet wiring, if there are any. Absent, unreadable or empty all mean the same thing to the caller: fall through the cascade.
void loadCredentials() {
    const char* label = nullptr;
    for (const auto& c : kFsCandidates) {
        if (!esp_partition_find_first(ESP_PARTITION_TYPE_DATA, c.subtype, c.label)) continue;
        esp_vfs_littlefs_conf_t conf = {};
        conf.base_path = kFsMountPoint;
        conf.partition_label = c.label;
        conf.format_if_mount_failed = false;   // never format: this volume is the user's config
        if (esp_vfs_littlefs_register(&conf) == ESP_OK) { label = c.label; break; }
    }
    if (!label) return;

    // The keys are a cross-image contract with what the app writes; the app pins it with a unit test (unit_MoonBaseContract).
    if (char* buf = readConfig(kNetworkConfig)) {
        // The rows of the WiFi child's known list, in order.
        while (networkCount_ < kMaxNetworks
               && mm::configscrape::findNetwork(buf, networkCount_, networks_[networkCount_].ssid, sizeof(Network::ssid),
                                                networks_[networkCount_].password, sizeof(Network::password)))
            networkCount_++;
        for (uint8_t k = 0; k < networkCount_; k++) networks_[k].ip = mm::configscrape::findNetworkIp(buf, k);
        ethIp_ = mm::configscrape::findChildIp(buf, "EthernetModule");
        mm::configscrape::findInt(buf, "ethType",       &ethCfg_.type);
        mm::configscrape::findInt(buf, "ethPhyAddr",    &ethCfg_.phyAddr);
        mm::configscrape::findInt(buf, "ethRstGpio",    &ethCfg_.rstGpio);
        mm::configscrape::findInt(buf, "ethMdcGpio",    &ethCfg_.mdcGpio);
        mm::configscrape::findInt(buf, "ethMdioGpio",   &ethCfg_.mdioGpio);
        mm::configscrape::findInt(buf, "ethClockGpio",  &ethCfg_.clockGpio);
        mm::configscrape::findBool(buf, "ethClockExtIn", &ethCfg_.clockExtIn);
        mm::configscrape::findInt(buf, "txPowerSetting", &txPowerDbm_);
        mm::configscrape::findChildString(buf, "AccessPointModule", "password", apPassword_, sizeof(apPassword_));
        std::free(buf);
    }
    // Before the unmount: this function owns the only window in which the volume is mounted.
    loadIdentity();
    esp_vfs_littlefs_unregister(label);
}

// ---------------------------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------------------------

// Pin a static address as the app does, once the link is up, when ESP-IDF lets DHCP stop; one the app refuses is refused here.
bool pinStatic(esp_netif_t* netif, const mm::configscrape::SavedIp& s) {
    if (!netif || !s.usable()) return false;
    esp_netif_dhcpc_stop(netif);
    esp_netif_ip_info_t info = {};
    IP4_ADDR(&info.ip,      s.ip[0],      s.ip[1],      s.ip[2],      s.ip[3]);
    IP4_ADDR(&info.gw,      s.gateway[0], s.gateway[1], s.gateway[2], s.gateway[3]);
    IP4_ADDR(&info.netmask, s.subnet[0],  s.subnet[1],  s.subnet[2],  s.subnet[3]);
    esp_netif_set_ip_info(netif, &info);
    if (s.dns[0] || s.dns[1] || s.dns[2] || s.dns[3]) {
        esp_netif_dns_info_t dns = {};
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        IP4_ADDR(&dns.ip.u_addr.ip4, s.dns[0], s.dns[1], s.dns[2], s.dns[3]);
        esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
    }
    return true;
}

esp_netif_t* ethNetif_ = nullptr;
esp_netif_t* staNetif_ = nullptr;
// The known network being tried, whose addressing its connection applies, and whether the one before pinned a static address.
volatile int staCurrent_ = -1;
volatile bool staPinned_ = false;

void onGotIp(void*, esp_event_base_t, int32_t id, void*) {
    // Registered for every IP event; only an acquired STA address means online (IP_EVENT_STA_LOST_IP arrives on the same base and must not set the bit).
    if (id == IP_EVENT_STA_GOT_IP || id == IP_EVENT_ETH_GOT_IP)
        xEventGroupSetBits(netEvents_, kNetGotIp);
}

void onWifiEvent(void*, esp_event_base_t, int32_t id, void*) {
    // Static has no lease to announce it, so the connection is the moment it is online.
    if (id == WIFI_EVENT_STA_CONNECTED && staCurrent_ >= 0) {
        const bool pinned = pinStatic(staNetif_, networks_[staCurrent_].ip);
        if (!pinned && staPinned_) esp_netif_dhcpc_start(staNetif_);   // a network after a static one leases again
        staPinned_ = pinned;
        if (pinned) xEventGroupSetBits(netEvents_, kNetGotIp);
    }
    if (id == WIFI_EVENT_STA_DISCONNECTED) xEventGroupSetBits(netEvents_, kNetStaDown);
    if (switchingNetwork_) return;
    if (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED) esp_wifi_connect();
}
#if SOC_EMAC_SUPPORTED
void onEthEvent(void*, esp_event_base_t, int32_t id, void*) {
    if (id == ETHERNET_EVENT_CONNECTED && pinStatic(ethNetif_, ethIp_)) xEventGroupSetBits(netEvents_, kNetGotIp);
}
#endif

// Bring up the on-chip MAC as the app's config wires it, staying installed without a link so a later cable still gets an address.
esp_eth_handle_t ethHandle_ = nullptr;

bool ethStart() {
#if !SOC_EMAC_SUPPORTED
    // No internal EMAC on this chip, so its recovery network is WiFi.
    return false;
#else
    if (ethCfg_.type != 1) return false;   // 1 = LAN8720/RMII in the app's ethType vocabulary

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t* netif = esp_netif_new(&netif_cfg);
    if (!netif) return false;
    esp_event_handler_instance_register(ETH_EVENT, ETHERNET_EVENT_CONNECTED, &onEthEvent, nullptr, nullptr);

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_config.clock_config.rmii.clock_mode = ethCfg_.clockExtIn ? EMAC_CLK_EXT_IN : EMAC_CLK_OUT;
    emac_config.clock_config.rmii.clock_gpio = static_cast<gpio_num_t>(ethCfg_.clockGpio);
    if (ethCfg_.mdcGpio >= 0)  emac_config.smi_gpio.mdc_num  = ethCfg_.mdcGpio;
    if (ethCfg_.mdioGpio >= 0) emac_config.smi_gpio.mdio_num = ethCfg_.mdioGpio;

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = ethCfg_.phyAddr;
    phy_config.reset_gpio_num = ethCfg_.rstGpio;

    esp_eth_mac_t* mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    esp_eth_phy_t* phy = mac ? esp_eth_phy_new_generic(&phy_config) : nullptr;
    if (!mac || !phy) {
        if (phy) phy->del(phy);
        if (mac) mac->del(mac);
        esp_netif_destroy(netif);
        return false;
    }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t handle = nullptr;
    if (esp_eth_driver_install(&eth_config, &handle) != ESP_OK) {
        phy->del(phy);
        mac->del(mac);
        esp_netif_destroy(netif);
        return false;
    }
    ethNetif_ = netif;   // before the start, since the link can come up before this returns
    if (esp_netif_attach(netif, esp_eth_new_netif_glue(handle)) != ESP_OK ||
        esp_eth_start(handle) != ESP_OK) {
        esp_eth_driver_uninstall(handle);   // frees mac + phy
        esp_netif_destroy(netif);
        ethNetif_ = nullptr;
        return false;
    }
    ethHandle_ = handle;
    return true;
#endif  // SOC_EMAC_SUPPORTED
}

// Tear Ethernet down again when no lease arrived in its window. Like the app, MoonBase runs ONE interface at a time, so WiFi only takes over from a dead link, never alongside it.
void ethStop() {
    if (!ethHandle_) return;
    esp_eth_stop(ethHandle_);
    esp_eth_driver_uninstall(ethHandle_);   // frees mac + phy
    esp_netif_destroy(ethNetif_);
    ethHandle_ = nullptr;
    ethNetif_ = nullptr;
}


// Try each known network in turn for a bounded time, as the app does. Returns whether an address arrived.
bool wifiStation(uint32_t waitMs) {
    if (!networkCount_) return false;
    staNetif_ = esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init) != ESP_OK) return false;
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &onWifiEvent, nullptr, nullptr);

    EventBits_t bits = 0;
    bool started = false;
    for (uint8_t k = 0; k < networkCount_ && !(bits & kNetGotIp); k++) {
        if (started) {
            // A station still connecting refuses a new config, so the attempt before is dropped, with its retry held, and gone before this one's config goes in.
            switchingNetwork_ = true;
            xEventGroupClearBits(netEvents_, kNetStaDown);
            esp_wifi_disconnect();
            xEventGroupWaitBits(netEvents_, kNetStaDown, pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));
        }
        wifi_config_t cfg = {};
        std::strncpy(reinterpret_cast<char*>(cfg.sta.ssid), networks_[k].ssid, sizeof(cfg.sta.ssid) - 1);
        std::strncpy(reinterpret_cast<char*>(cfg.sta.password), networks_[k].password, sizeof(cfg.sta.password) - 1);
        const bool configured = esp_wifi_set_config(WIFI_IF_STA, &cfg) == ESP_OK;
        staCurrent_ = k;   // each known network keeps its own addressing, as in the app
        switchingNetwork_ = false;
        if (!configured) continue;   // the next network, never the previous one under this one's turn
        // The first starts the radio, whose start event connects; each later one connects itself.
        if (!started) started = esp_wifi_start() == ESP_OK;
        else esp_wifi_connect();
        if (!started) continue;
        bits = xEventGroupWaitBits(netEvents_, kNetGotIp, pdFALSE, pdFALSE, pdMS_TO_TICKS(waitMs));
    }
    if (bits & kNetGotIp) {
        // Modem power save (the default, re-armed at association) throttles receive throughput to tens of KB/s. Disabled AFTER the connection is up so nothing re-enables it; MoonBase runs for minutes on a powered board, full RX beats the milliwatts.
        esp_wifi_set_ps(WIFI_PS_NONE);
        // The cap only once connected and only nonzero, since otherwise it hangs a classic ESP32; the access point skips it for that reason.
        if (txPowerDbm_ >= 1 && txPowerDbm_ <= 21) {
            esp_wifi_set_max_tx_power(static_cast<int8_t>(txPowerDbm_ * 4));
        }
        return true;
    }
    esp_wifi_stop();
    esp_wifi_deinit();
    return false;
}

// Answer every name with the access point's address, the only interface up when this runs, so a joining phone shows this page.
void captiveDnsTask(void*) {
    const int s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(53);
    if (s < 0 || ::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (s >= 0) ::close(s);
        vTaskDelete(nullptr);
        return;
    }
    uint8_t msg[mm::captive::kMaxMessage];
    while (true) {
        sockaddr_in from = {};
        socklen_t fromLen = sizeof(from);
        const int n = ::recvfrom(s, msg, sizeof(msg), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n <= 0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }   // an erroring socket must not starve the task serving the page
        const size_t len = mm::captive::dnsReply(msg, static_cast<size_t>(n), sizeof(msg), mm::captive::kAddress);
        if (len) ::sendto(s, msg, len, 0, reinterpret_cast<sockaddr*>(&from), fromLen);
    }
}

// The last resort, and the reason SoftAP stays in the size budget: a board whose stored credentials no longer work is still reachable without a cable.
bool wifiAccessPoint() {
    esp_netif_t* ap = esp_netif_create_default_wifi_ap();
    if (!ap) return false;
    esp_netif_ip_info_t ip = {};
    ip.ip.addr = esp_ip4addr_aton(mm::captive::kAddressText);   // the app's address, so a user finds the device where it was
    ip.gw.addr = ip.ip.addr;
    ip.netmask.addr = esp_ip4addr_aton("255.255.255.0");
    esp_netif_dhcps_stop(ap);
    esp_netif_set_ip_info(ap, &ip);
    // RFC 8910's option 114 names the portal, which a newer phone opens without probing; the server keeps the pointer, hence static.
    static char portalUri[] = "http://" MM_CAPTIVE_ADDRESS "/";
    esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, portalUri, sizeof(portalUri) - 1);
    esp_netif_dhcps_start(ap);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init) != ESP_OK) return false;
    wifi_config_t cfg = {};
    const char* name = apName_[0] ? apName_ : kApName;
    std::strncpy(reinterpret_cast<char*>(cfg.ap.ssid), name, sizeof(cfg.ap.ssid) - 1);
    cfg.ap.ssid_len = static_cast<uint8_t>(std::strlen(name));
    cfg.ap.max_connection = 2;
    // The app's WPA2 password where it set one, since a phone treats an open network under a known protected name as a stranger.
    const size_t pwLen = std::strlen(apPassword_);
    if (pwLen >= 8 && pwLen <= 63) {
        std::memcpy(cfg.ap.password, apPassword_, pwLen);
        cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        cfg.ap.authmode = WIFI_AUTH_OPEN;
    }
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &cfg);
    if (esp_wifi_start() != ESP_OK) return false;
    xTaskCreate(captiveDnsTask, "mb_dns", 3072, nullptr, 4, nullptr);   // without it the user still reaches 4.3.2.1 by typing it
    return true;
}

// ---------------------------------------------------------------------------------------------
// Installing
// ---------------------------------------------------------------------------------------------

// The one page this image serves, inline, and the chip it was built for, spelled as the release assets spell it.
#ifndef MOONBASE_CHIP
#define MOONBASE_CHIP CONFIG_IDF_TARGET
#endif

const char kPage[] =
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>MoonBase</title><link rel=icon href=/logo.png>"
    // The app UI's own palette (src/ui/style.css :root), so the two faces of one device match.
    "<style>body{font:16px system-ui;margin:2rem;max-width:34rem;line-height:1.5;"
    "background:#1a1a2e;color:#e0e0e0}"
    "h1{font-size:1.3rem;margin:0}.sub{color:#a0a0b0;margin-top:0}"
    "a{color:#a78bfa}"
    "input,button{font:inherit;padding:.5rem;background:#283661;color:#e0e0e0;"
    "border:1px solid #2a3a6a;border-radius:.35rem}button{cursor:pointer}"
    "section{margin:1.5rem 0;padding:1rem;border:1px solid #2a3a6a;border-radius:.5rem;"
    "background:#1f2c4f}"
    "#hdr{display:flex;align-items:center;gap:.7rem}#hdr img{width:40px;height:40px}"
    "#hlp{margin-left:auto;text-decoration:none;border:1px solid #2a3a6a;border-radius:.35rem;"
    "padding:.15rem .55rem}"
    "#s{margin-top:1rem;font-variant-numeric:tabular-nums}</style>"
    "<div id=hdr><img src=/logo.png alt=''><h1>MoonBase</h1>"
    // The (?) module cards carry, pointing at the published MoonBase doc.
    "<a id=hlp target=_blank rel=noopener title='MoonBase documentation' "
    "href='https://moonmodules.org/MoonLight/gettingstarted.html#if-your-device-shows-moonbase'>?</a></div>"
    "<p class=sub>Install firmware to return this device to normal operation."
    // Which MoonBase this is, filled from the image's own descriptor by the script below.
    "<br><small id=v></small></p>"
    "<section><b>From a file</b><br><input type=file id=f accept=.bin>"
    "<button onclick=up()>Install</button>"
    // The last resort when no URL is at hand: name where the firmware-<variant>-v*.bin files live. A plain link, so it works from any device that can reach the internet.
    "<br><small>Firmware files: <a href='https://github.com/MoonModules/MoonLight/releases' "
    "target=_blank rel=noopener>github.com/MoonModules/MoonLight/releases</a> "
    "(the firmware-...bin matching this board)</small></section>"
    // Installing from a release without typing a URL: the browser fetches the list, and this image only receives the URL.
    "<section><b>From a release</b><br>"
    "<select id=rel></select> <select id=fw></select> <button onclick=rl()>Install</button>"
    "<br><small id=rs></small></section>"
    "<section><b>From a URL</b><br><input id=u size=34 placeholder=https://...>"
    "<button onclick=url()>Install</button></section>"
    "<section><b>Back to the app</b><br>Boot the installed firmware without changing it."
    "<br><button onclick=ba()>Boot the app</button> "
    // Shown only while an install is running (S() toggles it): the one moment cancel applies.
    "<button id=c onclick=cx() style=display:none>Cancel install</button></section>"
    "<div id=s></div>"
    // A real bar, not a sweep: an install is a minute of a user watching a number they cannot read as a fraction. Hidden until a byte count actually arrives.
    "<progress id=p max=100 style='display:none;width:100%'></progress>"
    "<script>"
    // S() renders the status AND reveals Cancel only while an install is running.
    "const S=t=>{document.getElementById('s').textContent=t;"
    "document.getElementById('c').style.display="
    "/downloading|starting|preparing|retrying/.test(t)?'':'none';"
    // The fraction read out of the status's byte counts, the shape the app writes; unit_MoonBaseContract pins that both read it.
    "const m=/(\\d+) of (\\d+)/.exec(t),b=document.getElementById('p');"
    "if(m&&+m[2]>0){b.style.display='';b.value=100*m[1]/m[2];}else{b.style.display='none';}};"
    // Surface the last install status on load: after a failed unattended install the user lands here, and the page should say what went wrong rather than look freshly booted.
    "fetch('/moonbase').then(r=>r.text()).then(t=>{if(t&&t!='idle'){S(t);"
    // And watch an install the app staged, which nothing on this page started, so the page follows the app back.
    "if(/downloading|starting|preparing|retrying/.test(t))W();}}).catch(()=>{});"
    // A device that cannot say which MoonBase it runs cannot be diagnosed: two boards looked identical while one could not install firmware, and telling them apart took a git bisect.
    "fetch('/api/version').then(r=>r.text()).then(t=>{"
    "document.getElementById('v').textContent='version '+t}).catch(()=>{});"
    // The file as the raw request body, not multipart, so the device writes it straight to flash, as the app's upload route does.
    "async function up(){const f=document.getElementById('f').files[0];if(!f)return;"
    "S('installing '+(f.size/1024|0)+' KB...');"
    "const r=await fetch('/api/firmware/upload',{method:'POST',body:f});"
    "S(await r.text());}"
    // The install runs on its own task (202); W() watches its status until the app answers (404 on /moonbase means the new firmware is up, at this same address).
    "function W(){const t=setInterval(async()=>{try{const p=await fetch('/moonbase');"
    "if(p.status==404){clearInterval(t);S('done, the app is starting...');"
    "setTimeout(()=>location.reload(),3000);}else{S(await p.text());}}"
    "catch(_){S('restarting...');}},2000);}"
    // The release list, filtered to the assets this chip runs, from which the user picks the variant.
    "const CHIP='" MOONBASE_CHIP "';let RELS=[],VAR='';"
    // With the variant the app persisted, the one firmware this board takes; without it, every firmware for the chip rather than a guess.
    "fetch('/api/variant').then(r=>r.text()).then(t=>{VAR=t.trim();fillFw();}).catch(()=>{});"
    "function fwList(i){const r=RELS[i];if(!r)return [];"
    "return (r.assets||[]).map(a=>a.name).filter(n=>/^firmware-.+\\.bin$/.test(n)"
    "&&!/-(bootloader|partition-table|ota-data|slot0)\\.bin$/.test(n)"
    // The chip matches to a boundary, a hyphen or the P4's revision suffix, since one target name is a prefix of another's.
    "&&n.slice(9).startsWith(CHIP)"
    "&&(n.slice(9+CHIP.length).startsWith('-')||/^rev\\d/.test(n.slice(9+CHIP.length)))"
    "&&(!VAR||n.slice(9).startsWith(VAR+'-')));}"
    "function fillFw(){const f=document.getElementById('fw');f.innerHTML='';"
    "const l=fwList(document.getElementById('rel').selectedIndex);"
    "for(const n of l){const o=document.createElement('option');o.textContent=n;f.appendChild(o);}"
    "document.getElementById('rs').textContent=l.length?'':'no firmware for this chip in that release';}"
    "fetch('https://api.github.com/repos/MoonModules/MoonLight/releases?per_page=10')"
    ".then(r=>r.json()).then(j=>{RELS=j;const s=document.getElementById('rel');"
    "for(const r of RELS){const o=document.createElement('option');"
    "o.textContent=(r.name||r.tag_name)+(r.prerelease?' (pre)':'');s.appendChild(o);}"
    "s.onchange=fillFw;fillFw();})"
    ".catch(()=>{document.getElementById('rs').textContent="
    "'could not reach github: use a URL or a file below';});"
    // Installing a release is installing its URL: one path, so the vetting, the progress and the retry all behave identically however the URL was chosen.
    "async function rl(){const r=RELS[document.getElementById('rel').selectedIndex];"
    "const n=document.getElementById('fw').value;if(!r||!n)return;"
    "const a=(r.assets||[]).find(x=>x.name===n);if(!a)return;"
    "document.getElementById('u').value=a.browser_download_url;url();}"
    "async function url(){const u=document.getElementById('u').value;if(!u)return;"
    "const r=await fetch('/api/firmware/url',{method:'POST',body:u});S(await r.text());if(r.ok)W();}"
    // Prefill the URL field with the last install source (RAM-held), so Install doubles as retry: the escape after a cancel or failure wiped the app slot.
    "fetch('/api/firmware/last-url').then(r=>r.text()).then(u=>{if(u)document.getElementById('u').value=u;})"
    ".catch(()=>{});"
    // Reload when the app answers rather than after a fixed wait: the app 404s /moonbase, so a 404 means it is up.
    "async function ba(){const r=await fetch('/api/firmware/boot-app',{method:'POST'});S(await r.text());"
    "if(!r.ok)return;S('booting the app...');"
    "for(let i=0;i<60;i++){await new Promise(f=>setTimeout(f,1000));"
    "try{const p=await fetch('/moonbase',{cache:'no-store'});"
    "if(p.status==404){location.reload();return;}}catch(e){}}"
    "S('the app is not answering: it may not be installed');}"
    "async function cx(){S(await (await fetch('/api/firmware/cancel',{method:'POST'})).text());}"
    "</script>";

// The app's build variant for the page, empty before the app has run.

// The application slot, or null when this image is itself in it, installed as the app, so a caller says so rather than loops.
const esp_partition_t* appPartition() {
    const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
    return (part && part == esp_ota_get_running_partition()) ? nullptr : part;
}

// True while an install writes the app slot; a torn read is harmless, since the guard only stops a second install from starting.
volatile bool installing_ = false;
// Set by POST /cancel; the install loops poll it and abort cleanly back to the page. The app slot is left half-written, exactly like a power cut: MoonBase stays the boot target until a later install completes.
volatile bool cancelRequested_ = false;

bool installFromUrl(const char* url) {
    esp_http_client_config_t http = {};
    http.url = url;
    http.timeout_ms = 20000;
    http.keep_alive_enable = true;
    http.crt_bundle_attach = esp_crt_bundle_attach;   // GitHub and friends are HTTPS
    // A release asset's redirect carries headers past the default 512-byte buffer, so these match the app's http_fetch_to_ota.
    http.disable_auto_redirect = false;
    http.max_redirection_count = 10;
    // 32 KB chunks: measured on a classic ESP32 at 46 KB/s for 4 KB, 86 KB/s for 16, flat beyond.
    http.buffer_size = 32768;
    http.buffer_size_tx = 4096;
    esp_https_ota_config_t ota = {};
    ota.http_config = &http;
    // One bulk erase up front, since per-sector erases held the install to 25 KB/s; "preparing the install" covers the seconds it takes.
    ota.bulk_flash_erase = true;

    esp_https_ota_handle_t handle = nullptr;
    esp_err_t beginErr = esp_https_ota_begin(&ota, &handle);
    if (beginErr != ESP_OK) {
        // Numeric on purpose: the error-name table is compiled out for size (ESP_ERR_TO_NAME_LOOKUP=n), so esp_err_to_name would say "UNKNOWN ERROR".
        std::snprintf(status_, sizeof(status_), "error: cannot start the download (0x%x)",
                      static_cast<unsigned>(beginErr));
        return false;
    }
    // Not another recovery image: one in the app slot leaves both partitions holding it, recoverable only with a cable.
    esp_app_desc_t incoming = {};
    if (esp_https_ota_get_img_desc(handle, &incoming) == ESP_OK &&
        mm::firmware::isMoonBaseImage(incoming.project_name, sizeof(incoming.project_name))) {
        std::snprintf(status_, sizeof(status_), "error: that is a MoonBase image, not an app");
        esp_https_ota_abort(handle);
        return false;
    }

    esp_err_t err;
    int lastGot = -1;
    TickType_t movedAt = xTaskGetTickCount();
    while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        if (cancelRequested_) {
            esp_https_ota_abort(handle);
            std::snprintf(status_, sizeof(status_), "canceled");
            return false;
        }
        // Give up on a connection that stopped delivering, which ESP-IDF reports as still in progress: FirmwareImage.h names why.
        const int got = esp_https_ota_get_image_len_read(handle);
        if (got != lastGot) {
            lastGot = got;
            movedAt = xTaskGetTickCount();
        } else if (pdTICKS_TO_MS(xTaskGetTickCount() - movedAt) > mm::firmware::kDownloadStallMs) {
            esp_https_ota_abort(handle);
            std::snprintf(status_, sizeof(status_), "error: the download stalled");
            return false;
        }
        std::snprintf(status_, sizeof(status_), "downloading: %d of %d bytes",
                      got, esp_https_ota_get_image_size(handle));
    }
    if (err != ESP_OK) {
        esp_https_ota_abort(handle);   // finish() is for a COMPLETE download; abort frees this one
        std::snprintf(status_, sizeof(status_), "error: the download failed (0x%x)",
                      static_cast<unsigned>(err));
        return false;
    }
    if (esp_https_ota_finish(handle) != ESP_OK) {
        std::snprintf(status_, sizeof(status_), "error: the image is not valid firmware");
        return false;
    }
    std::snprintf(status_, sizeof(status_), "installed, restarting");
    return true;
}


// The HTTP server on raw sockets, one connection at a time: one page and one file cost less than the framework component.

constexpr size_t kRecvChunk = 4096;

void sendAll(int sock, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        const int n = ::send(sock, data + sent, len - sent, 0);
        if (n <= 0) return;   // peer gone: the caller is finishing anyway
        sent += static_cast<size_t>(n);
    }
}

// The embedded logo (EMBED_FILES in CMakeLists; symbol names derive from the filename).
extern const uint8_t logoStart[] asm("_binary_moonmodules_logo_png_start");
extern const uint8_t logoEnd[]   asm("_binary_moonmodules_logo_png_end");

void sendBinary(int sock, const char* type, const uint8_t* data, size_t len) {
    char head[192];
    const int n = std::snprintf(head, sizeof(head),
                                "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
                                "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                                type, static_cast<unsigned>(len));
    if (n > 0) sendAll(sock, head, static_cast<size_t>(n));
    sendAll(sock, reinterpret_cast<const char*>(data), len);
}

void sendResponse(int sock, const char* status, const char* type, const char* body) {
    // no-store on everything: this address serves TWO different UIs over time (the app's and this one), and a browser that re-serves a cached copy of either shows a dead page.
    char head[192];
    const int n = std::snprintf(head, sizeof(head),
                                "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
                                "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                                status, type, static_cast<unsigned>(std::strlen(body)));
    if (n > 0) sendAll(sock, head, static_cast<size_t>(n));
    sendAll(sock, body, std::strlen(body));
}

// Write `contentLen` bytes from the socket into the application slot. `prefix` carries whatever arrived in the same read as the headers.
bool installFromSocketLocked(int sock, const char* prefix, size_t prefixLen, size_t contentLen) {
    const esp_partition_t* part = appPartition();
    if (!part) {
        // Either no OTA slot at all, or this MoonBase is running FROM it (see appPartition): the second is what a user meets, and a cable is the only way back.
        std::snprintf(status_, sizeof(status_),
                      "error: this MoonBase is in the app slot; reflash over USB");
        return false;
    }
    if (contentLen == 0 || contentLen > part->size) {
        std::snprintf(status_, sizeof(status_), "error: image is %u bytes, the slot holds %u",
                      static_cast<unsigned>(contentLen), static_cast<unsigned>(part->size));
        return false;
    }

    esp_ota_handle_t handle = 0;
    if (esp_ota_begin(part, contentLen, &handle) != ESP_OK) {
        std::snprintf(status_, sizeof(status_), "error: cannot start the install");
        return false;
    }

    size_t written = 0;
    if (prefixLen > contentLen) prefixLen = contentLen;   // never store bytes past the declared body
    // The URL path's refusal, from the bytes in hand, refusing a prefix too short to identify rather than passing it.
    if (prefixLen < mm::firmware::kIdentifyBytes) {
        esp_ota_abort(handle);
        std::snprintf(status_, sizeof(status_), "error: could not identify the image");
        return false;
    }
    const auto incomingUp = mm::firmware::identify(
        reinterpret_cast<const uint8_t*>(prefix), prefixLen);
    if (incomingUp.described && mm::firmware::isMoonBaseImage(incomingUp.project)) {
        esp_ota_abort(handle);
        std::snprintf(status_, sizeof(status_), "error: that is a MoonBase image, not an app");
        return false;
    }
    if (prefixLen) {
        if (esp_ota_write(handle, prefix, prefixLen) != ESP_OK) {
            esp_ota_abort(handle);
            std::snprintf(status_, sizeof(status_), "error: write failed");
            return false;
        }
        written = prefixLen;
    }

    char* buf = static_cast<char*>(std::malloc(kRecvChunk));
    if (!buf) { esp_ota_abort(handle); std::snprintf(status_, sizeof(status_), "error: out of memory"); return false; }
    while (written < contentLen) {
        const size_t want = (contentLen - written) < kRecvChunk ? (contentLen - written) : kRecvChunk;
        const int n = ::recv(sock, buf, want, 0);
        if (n <= 0) break;                       // the upload was cut short
        if (esp_ota_write(handle, buf, static_cast<size_t>(n)) != ESP_OK) {
            std::free(buf);
            esp_ota_abort(handle);
            std::snprintf(status_, sizeof(status_), "error: write failed");
            return false;
        }
        written += static_cast<size_t>(n);
    }
    std::free(buf);

    if (written != contentLen) {
        esp_ota_abort(handle);
        std::snprintf(status_, sizeof(status_), "error: upload ended early (%u of %u bytes)",
                      static_cast<unsigned>(written), static_cast<unsigned>(contentLen));
        return false;
    }
    // esp_ota_end validates the image (magic and checksum) before we ever point the bootloader at it, which is what makes a power cut mid-write safe: otadata still names MoonBase.
    if (esp_ota_end(handle) != ESP_OK) {
        std::snprintf(status_, sizeof(status_), "error: the image is not valid firmware");
        return false;
    }
    if (esp_ota_set_boot_partition(part) != ESP_OK) {
        std::snprintf(status_, sizeof(status_), "error: cannot set the boot partition");
        return false;
    }
    std::snprintf(status_, sizeof(status_), "installed, restarting");
    return true;
}

// Read the request head, dispatch, and on a successful install restart into the application.

// The staged-URL install, off the serving task, retrying briefly since a connect right after a fresh association can fail.
char stagedUrlTask_[256];

// How many installs a staged URL may start: a reset before one starts costs nothing, and an install that keeps resetting the device ends here.
constexpr uint8_t kMaxStagedInstalls = 3;

// Forget the staged install, its URL and its count, once an install has ended or the user chose another way.
void clearStagedInstall() {
    nvs_handle_t h;
    if (nvs_open("moonbase", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, "url");
    nvs_erase_key(h, "tries");
    nvs_commit(h);
    nvs_close(h);
}

void unattendedInstallTask(void*) {
    // Remember the source across reboots (key "last_url", page prefill only): the retry escape must survive a power cycle, not just this session.
    nvs_handle_t nh;
    if (nvs_open("moonbase", NVS_READWRITE, &nh) == ESP_OK) {
        nvs_set_str(nh, "last_url", stagedUrlTask_);
        nvs_commit(nh);
        nvs_close(nh);
    }
    for (int attempt = 0; attempt < 3 && !cancelRequested_; attempt++) {
        if (attempt) vTaskDelay(pdMS_TO_TICKS(3000));
        if (installFromUrl(stagedUrlTask_)) { clearStagedInstall(); esp_restart(); }   // straight back into the new app
        // A failed attempt leaves its error in status_; while retries remain that error is TRANSIENT, and a watcher treating "error:" as terminal (the app's overlay does) must not see it. The final attempt's error stays as the terminal answer.
        if (attempt < 2 && !cancelRequested_)
            std::snprintf(status_, sizeof(status_), "download failed, retrying");
    }
    clearStagedInstall();   // failed or canceled, and said so on the page: the user takes it from here
    cancelRequested_ = false;
    installing_ = false;   // set by the spawner; held across the retries
    vTaskDelete(nullptr);
}

void serveOne(int sock) {
    // TCP does not coalesce: the header block (or a small body) can arrive in several segments, so read until the blank line is seen, bounded by the buffer. A request whose headers do not fit 1023 bytes is not one of ours and falls out as 404.
    char head[1024];
    size_t got = 0;
    const char* bodyStart = nullptr;
    while (got < sizeof(head) - 1) {
        const int n = ::recv(sock, head + got, sizeof(head) - 1 - got, 0);
        if (n <= 0) break;
        got += static_cast<size_t>(n);
        head[got] = '\0';
        if ((bodyStart = std::strstr(head, "\r\n\r\n"))) break;
    }
    if (got == 0) { ::close(sock); return; }   // serveOne owns the fd; a bare return leaks it
    head[got] = '\0';
    const size_t headLen = bodyStart ? static_cast<size_t>(bodyStart + 4 - head) : got;
    size_t prefixLen = got - headLen;

    // HTTP header names are case-insensitive; strcasestr is not in the std namespace but is provided by newlib, and the probe is bounded by the header buffer.
    size_t contentLen = 0;
    if (const char* cl = strcasestr(head, "Content-Length:")) {
        contentLen = static_cast<size_t>(std::strtoul(cl + 15, nullptr, 10));
    }

    // A phone on the access point asking for its own captive-check page gets this one, which is what shows it the sign-in screen.
    if (std::strncmp(head, "GET ", 4) == 0) {
        sockaddr_in local = {};
        socklen_t localLen = sizeof(local);
        const char* host = strcasestr(head, "\r\nHost:");
        if (host) { host += 7; while (*host == ' ') host++; }
        if (host && ::getsockname(sock, reinterpret_cast<sockaddr*>(&local), &localLen) == 0 &&
            mm::captive::redirects(reinterpret_cast<const uint8_t*>(&local.sin_addr.s_addr), host, head + 4)) {
            ::send(sock, mm::captive::kRedirect, std::strlen(mm::captive::kRedirect), 0);
            ::shutdown(sock, SHUT_RDWR);
            ::close(sock);
            return;
        }
    }

    bool installed = false;
    if (std::strncmp(head, "POST /api/firmware/url", 22) == 0 && installing_) {
        sendResponse(sock, "409 Conflict", "text/plain", "error: an install is already running");
    } else if (std::strncmp(head, "POST /api/firmware/url", 22) == 0) {
        // The body is the URL itself; small enough to finish reading into the same buffer.
        while (prefixLen < contentLen && headLen + prefixLen < sizeof(head) - 1) {
            const int n = ::recv(sock, head + headLen + prefixLen,
                                 sizeof(head) - 1 - headLen - prefixLen, 0);
            if (n <= 0) break;
            prefixLen += static_cast<size_t>(n);
        }
        if (contentLen >= sizeof(stagedUrlTask_)) {
            // Same 255-byte contract the app's route enforces (platform.h): refusing beats truncating into a URL that fails later as a misleading download error.
            sendResponse(sock, "400 Bad Request", "text/plain", "error: url too long (max 255)");
        } else {
            std::memcpy(stagedUrlTask_, head + headLen, prefixLen);
            stagedUrlTask_[prefixLen] = '\0';
            std::snprintf(status_, sizeof(status_), "starting the install");
            cancelRequested_ = false;   // a /cancel racing the previous task's exit must not latch
            installing_ = true;   // cleared by the task after its final attempt
            if (xTaskCreate(unattendedInstallTask, "mb_install", 12288, nullptr, 5, nullptr) != pdPASS) {
                // A failed spawn with the flag left set would refuse every later install: THE deadlock this guard exists to prevent.
                installing_ = false;
                std::snprintf(status_, sizeof(status_), "error: cannot start the install task");
                sendResponse(sock, "500 Internal Server Error", "text/plain", status_);
            } else {
                // 202: the install runs on its own task while this server keeps answering GET /moonbase with live progress; the caller watches that, not this response.
                sendResponse(sock, "202 Accepted", "text/plain", status_);
            }
        }
    } else if (std::strncmp(head, "POST /api/firmware/upload", 25) == 0) {
        if (installing_) {
            sendResponse(sock, "409 Conflict", "text/plain", "error: an install is already running");
        } else {
            installing_ = true;
            installed = installFromSocketLocked(sock, head + headLen, prefixLen, contentLen);
            installing_ = false;
            sendResponse(sock, installed ? "200 OK" : "500 Internal Server Error", "text/plain", status_);
        }
    } else if (std::strncmp(head, "POST /api/firmware/boot-app", 27) == 0 && installing_) {
        // Booting away mid-write would abandon a half-written slot; refuse, visibly.
        sendResponse(sock, "409 Conflict", "text/plain", "error: an install is already running");
    } else if (std::strncmp(head, "POST /api/firmware/boot-app", 27) == 0) {
        // Switch back to the installed application without installing anything. esp_ota_set_boot_partition validates the image first, so a half-written app is refused and the device stays here. Only a bootable app can be booted.
        const esp_partition_t* app = appPartition();
        const bool ok = app && esp_ota_set_boot_partition(app) == ESP_OK;
        if (ok) std::snprintf(status_, sizeof(status_), "booting the app");
        else if (!app) {
            // appPartition returns null when THIS image is the one in the app slot. Before this, boot-app pointed the bootloader at itself and reported success, so the device "rebooted into the app" and arrived back here, forever.
            std::snprintf(status_, sizeof(status_),
                          "error: this MoonBase is in the app slot; reflash over USB");
        }
        else    std::snprintf(status_, sizeof(status_), "error: no valid app image");
        sendResponse(sock, ok ? "200 OK" : "500 Internal Server Error", "text/plain", status_);
        installed = ok;   // reuse the reply-then-restart tail below
    } else if (std::strncmp(head, "GET /logo.png", 13) == 0) {
        sendBinary(sock, "image/png", logoStart, static_cast<size_t>(logoEnd - logoStart));
    } else if (std::strncmp(head, "GET /api/firmware/last-url", 26) == 0) {
        // The most recent install source, RAM-held: the page prefills its URL field with it, so Install doubles as retry, the escape after a cancel wiped the app slot. Empty after a power cycle.
        sendResponse(sock, "200 OK", "text/plain", stagedUrlTask_);
    } else if (std::strncmp(head, "POST /api/firmware/cancel", 25) == 0) {
        // Cancel a running URL install: its loop polls the flag and aborts back to this page. (An upload cancels by dropping the connection; this server is busy receiving it.) Nothing to cancel is not an error worth a scary status, just say so.
        if (installing_) {
            cancelRequested_ = true;
            sendResponse(sock, "200 OK", "text/plain", "canceling");
        } else {
            sendResponse(sock, "200 OK", "text/plain", "nothing to cancel");
        }
    } else if (std::strncmp(head, "GET /api/variant", 16) == 0) {
        // The application's build variant, read from its config at boot. Empty when the app has never run here, which the page treats as "offer every firmware for the chip".
        sendResponse(sock, "200 OK", "text/plain", g_appVariant);
    } else if (std::strncmp(head, "GET /api/version", 16) == 0) {
        // This image's version from its descriptor, on its own route, since the app parses the status route's body.
        const esp_app_desc_t* d = esp_app_get_description();
        sendResponse(sock, "200 OK", "text/plain", d ? d->version : "unknown");
    } else if (std::strncmp(head, "GET /moonbase", 13) == 0) {
        // Identity probe: the app UI polls this across the update cycle to tell which image is answering at the shared address (the app 404s it). Body = the live install status, so the poll doubles as a progress read during an unattended install.
        sendResponse(sock, "200 OK", "text/plain", status_);
    } else if (std::strncmp(head, "GET / ", 6) == 0 || std::strncmp(head, "GET /?", 6) == 0 ||
               std::strncmp(head, "GET /index", 10) == 0) {
        // "/?<ts>" is the app page's cache-busting handoff to this page (app.js).
        sendResponse(sock, "200 OK", "text/html", kPage);
    } else {
        sendResponse(sock, "404 Not Found", "text/plain", "not found");
    }

    ::shutdown(sock, SHUT_RDWR);
    ::close(sock);
    if (installed) {
        clearStagedInstall();   // an upload or Boot the app supersedes whatever the app staged
        // Let the reply reach the browser before the device goes away.
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
}

void serveForever() {
    const int listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener < 0) return;
    int yes = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(kHttpPort);
    if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) { ::close(listener); return; }
    if (::listen(listener, 1) != 0) { ::close(listener); return; }

    while (true) {
        const int sock = ::accept(listener, nullptr, nullptr);
        if (sock < 0) continue;
        // A stalled peer must not hold MoonBase forever: the whole point is that the device stays reachable for the next attempt.
        timeval tv = {};
        tv.tv_sec = 30;
        ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        serveOne(sock);
    }
}

}  // namespace

extern "C" void app_main() {
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    netEvents_ = xEventGroupCreate();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &onGotIp, nullptr, nullptr);

    loadCredentials();   // also reads the app's build variant, inside its mount window

    // The cascade: Ethernet as the config wires it, the known networks in order, then the device's own access point, so stale credentials never strand a board.

    // An install URL the app staged, kept until an install ends, so a reset before or during the download retries it on the next boot.
    char stagedUrl[256] = {};
    uint8_t stagedTries = 0;
    {
        nvs_handle_t h;
        if (nvs_open("moonbase", NVS_READONLY, &h) == ESP_OK) {
            size_t len = sizeof(stagedUrl);
            if (nvs_get_str(h, "url", stagedUrl, &len) != ESP_OK) stagedUrl[0] = '\0';
            nvs_get_u8(h, "tries", &stagedTries);
            nvs_close(h);
        }
    }
    if (stagedUrl[0] && stagedTries >= kMaxStagedInstalls) {
        clearStagedInstall();
        std::snprintf(status_, sizeof(status_), "error: the staged install reset the device %u times; install from this page",
                      static_cast<unsigned>(kMaxStagedInstalls));
        stagedUrl[0] = '\0';
    }

    // With nothing staged, prefill the retry buffer from the remembered last source so the page offers it after any reboot. Never auto-installed: only the page's Install uses it.
    if (!stagedUrl[0]) {
        nvs_handle_t h;
        if (nvs_open("moonbase", NVS_READONLY, &h) == ESP_OK) {
            size_t len = sizeof(stagedUrlTask_);
            if (nvs_get_str(h, "last_url", stagedUrlTask_, &len) != ESP_OK) stagedUrlTask_[0] = '\0';
            nvs_close(h);
        }
    }

    // One interface at a time, in the app's order, so the browser finds this image at the address the app had.
    bool online = false;
    if (ethStart()) {
        online = (xEventGroupWaitBits(netEvents_, kNetGotIp, pdFALSE, pdFALSE,
                                      pdMS_TO_TICKS(8000)) & kNetGotIp) != 0;
        if (!online) {
            ethStop();   // no link or no lease: WiFi takes over, alone
            // A lease that raced in between the wait timing out and the teardown is an interface that no longer exists; it must not satisfy the WiFi wait below.
            xEventGroupClearBits(netEvents_, kNetGotIp);
        }
    }
    // 10 s a network as the app gives each, and the whole 20 s to a lone one.
    if (!online) online = wifiStation(networkCount_ > 1 ? 10000 : 20000);

    // Online only, the access point reaching no URL; on its own task with 12 KB for TLS, so /moonbase reports progress meanwhile.
    if (online && stagedUrl[0]) {
        // Counted as it starts, after the network is up, so only an install that resets the device spends a try.
        nvs_handle_t h;
        if (nvs_open("moonbase", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_u8(h, "tries", static_cast<uint8_t>(stagedTries + 1));
            nvs_commit(h);
            nvs_close(h);
        }
        // Status set BEFORE the task spawns: the overlay polls from the moment MoonBase answers, and "idle" would read as nothing happening while an install is pending.
        std::snprintf(status_, sizeof(status_), "preparing the install");
        std::snprintf(stagedUrlTask_, sizeof(stagedUrlTask_), "%s", stagedUrl);
        cancelRequested_ = false;   // a /cancel racing a previous task's exit must not latch
        installing_ = true;   // cleared by the task after its final attempt
        if (xTaskCreate(unattendedInstallTask, "mb_install", 12288, nullptr, 5, nullptr) != pdPASS) {
            installing_ = false;   // a latched flag would refuse every later install
            std::snprintf(status_, sizeof(status_), "error: cannot start the install task");
        }
    }

    if (!online) online = wifiAccessPoint();

    // With no network there is nothing MoonBase can do but wait. A user who cannot reach it will reflash over USB, and restarting into an application slot that may be empty helps nobody.
    if (online) serveForever();
    while (true) vTaskDelay(pdMS_TO_TICKS(1000));
}
