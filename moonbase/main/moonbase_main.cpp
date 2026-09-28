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
#include "core/util/FirmwareImage.h"  // identify(): the one shared header, see main/CMakeLists.txt
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

// The application writes its config as /.config/<TypeName>.json on a LittleFS volume.
// Tables written from 2026-08 label that partition `littlefs`; older ones label it `spiffs`, so both are tried by subtype then label.
// MoonBase only ever READS it, so a failed install cannot corrupt user config.
struct FsCandidate { esp_partition_subtype_t subtype; const char* label; };
constexpr FsCandidate kFsCandidates[] = {
    {ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, "littlefs"},
    {ESP_PARTITION_SUBTYPE_DATA_SPIFFS,   "spiffs"},
};
constexpr const char* kFsMountPoint     = "/fs";
constexpr const char* kNetworkConfig    = "/fs/.config/NetworkModule.json";
// The application persists its build variant here, which is the one fact this image cannot know about the board it is on. MoonBase is chip-specific and variant-agnostic, so without it the release picker can only offer every firmware for the chip and ask a user in recovery to choose.
constexpr const char* kSystemConfig     = "/fs/.config/SystemModule.json";

// The AP fallback address matches the application's (NetworkModule uses 4.3.2.1), so a user who has provisioned this device before sees the same address in both firmwares.
constexpr const char* kApAddress = "4.3.2.1";
constexpr const char* kApName    = "MoonBase";

constexpr int kHttpPort = 80;

char ssid_[64] = {};
char password_[64] = {};
char status_[96] = "idle";

EventGroupHandle_t netEvents_;
constexpr int kNetGotIp = BIT0;

// ---------------------------------------------------------------------------------------------
// Credentials
// ---------------------------------------------------------------------------------------------

// One top-level string out of the config, deliberately not a parser: two known keys from a file this project wrote, where linking one would cost more than the feature. Anchored at the top level, because the same file carries a child module's password a naive search would find.
bool jsonFindString(const char* json, const char* key, char* out, size_t outLen) {
    char needle[40];
    const int n = std::snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(needle)) return false;
    const char* p = std::strstr(json, needle);
    if (!p) return false;
    p += n;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < outLen) {
        char c = *p++;
        if (c == '\\' && *p) {
            // The app's writer (JsonSink, RFC 8259) escapes with \" \\ \/ \n \r \t, and \uXXXX for other control bytes.
            // All but \u are decoded here.
            // A credential holding a raw control byte fails the join and lands on the access point, visible and recoverable, which is not worth a \u decoder in this image.
            const char e = *p++;
            switch (e) {
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'u': return false;
                default:  c = e;      // \" \\ \/ decode to the char itself
            }
        }
        out[i++] = c;
    }
    out[i] = '\0';
    return i > 0;
}

// Top-level numeric key: "key":123 or "key":-1 (same anchored scan as jsonFindString). Absent leaves `out` untouched, so callers pre-load their defaults.
void jsonFindInt(const char* json, const char* key, int* out) {
    char needle[40];
    const int n = std::snprintf(needle, sizeof(needle), "\"%s\":", key);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(needle)) return;
    const char* v = std::strstr(json, needle);
    if (!v) return;
    v += n;
    if (*v == '-' || (*v >= '0' && *v <= '9')) *out = std::atoi(v);
}

void jsonFindBool(const char* json, const char* key, bool* out) {
    char needle[40];
    const int n = std::snprintf(needle, sizeof(needle), "\"%s\":", key);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(needle)) return;
    const char* v = std::strstr(json, needle);
    if (!v) return;
    v += n;
    if (std::strncmp(v, "true", 4) == 0)  *out = true;
    if (std::strncmp(v, "false", 5) == 0) *out = false;
}

// The board's Ethernet wiring, from the same config file the credentials come from. ethType 1 is the app's LAN8720/RMII option, the only interface a 4 MB classic has.
// 0 or absent means no Ethernet on this board.
// Absent pin keys keep the silicon defaults (MDC 23 / MDIO 18; clock IN on GPIO0), the same rule the app applies.
struct {
    int  type       = 0;
    int  phyAddr    = -1;      // -1: scan the MDIO bus
    int  rstGpio    = -1;
    int  mdcGpio    = -1;      // <0: leave the EMAC default
    int  mdioGpio   = -1;
    int  clockGpio  = 0;
    bool clockExtIn = true;
} ethCfg_;

// The board's WiFi TX cap in dBm (0 = no override): some assemblies brown out at full TX (the catalog pins e.g. 8 dBm for them), and a brownout during recovery is the worst time.
int txPowerDbm_ = 0;

// The application's build variant ("esp32s3-zero"), or empty when the app has never run here.
// Empty is the honest answer for a freshly flashed or wiped device.
// The page falls back to offering every firmware for the chip rather than pretending to know which one this board takes.
char g_appVariant[24] = {};

void loadAppVariant() {
    FILE* f = std::fopen(kSystemConfig, "r");
    if (!f) return;
    // Same bounded prefix read as the credentials above, and for the same reason: the file carries every child module's config behind the identity keys this image needs.
    char buf[1024];
    const size_t got = std::fread(buf, 1, sizeof(buf) - 1, f);
    buf[got] = '\0';
    std::fclose(f);
    jsonFindString(buf, "firmware", g_appVariant, sizeof(g_appVariant));
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

    FILE* f = std::fopen(kNetworkConfig, "r");
    if (f) {
        // The credentials are the first keys the module writes, so a bounded prefix read finds them without holding the whole file (which carries every child module's config too). The bound is a cross-image contract with NetworkModule's control order; the app pins it with a unit test (unit_MoonBaseContract).
        char buf[2048];
        const size_t got = std::fread(buf, 1, sizeof(buf) - 1, f);
        buf[got] = '\0';
        std::fclose(f);
        jsonFindString(buf, "ssid", ssid_, sizeof(ssid_));
        jsonFindString(buf, "password", password_, sizeof(password_));
        jsonFindInt(buf, "ethType",       &ethCfg_.type);
        jsonFindInt(buf, "ethPhyAddr",    &ethCfg_.phyAddr);
        jsonFindInt(buf, "ethRstGpio",    &ethCfg_.rstGpio);
        jsonFindInt(buf, "ethMdcGpio",    &ethCfg_.mdcGpio);
        jsonFindInt(buf, "ethMdioGpio",   &ethCfg_.mdioGpio);
        jsonFindInt(buf, "ethClockGpio",  &ethCfg_.clockGpio);
        jsonFindBool(buf, "ethClockExtIn", &ethCfg_.clockExtIn);
        jsonFindInt(buf, "txPowerSetting", &txPowerDbm_);
    }
    // BEFORE THE UNMOUNT.
    // This function owns the only window in which the volume is mounted.
    // It registers the partition above and unregisters it here, so anything that reads a file has to do it now.
    // Reading the variant after this call returned "no file" for exactly that reason.
    loadAppVariant();
    esp_vfs_littlefs_unregister(label);
}

// ---------------------------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------------------------

void onGotIp(void*, esp_event_base_t, int32_t id, void*) {
    // Registered for every IP event; only an acquired STA address means online (IP_EVENT_STA_LOST_IP arrives on the same base and must not set the bit).
    if (id == IP_EVENT_STA_GOT_IP || id == IP_EVENT_ETH_GOT_IP)
        xEventGroupSetBits(netEvents_, kNetGotIp);
}

void onWifiEvent(void*, esp_event_base_t, int32_t id, void*) {
    if (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED) esp_wifi_connect();
}
// Bring up the on-chip MAC with the wiring the app's config names.
// Fire-and-forget by design: the driver stays installed when no link appears in the window, so a cable plugged in later still gets an address.
// False only when nothing was configured or a step failed.
esp_eth_handle_t ethHandle_ = nullptr;
esp_netif_t* ethNetif_ = nullptr;

bool ethStart() {
#if !SOC_EMAC_SUPPORTED
    // No internal EMAC on this chip (the S3 and other WiFi-only parts).
    // MoonBase's job is to get a recovery UI onto the network, and on such a board that is WiFi.
    // The RMII path below would not link, and its esp_eth_mac_new_esp32 does not even exist there.
    return false;
#else
    if (ethCfg_.type != 1) return false;   // 1 = LAN8720/RMII in the app's ethType vocabulary

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t* netif = esp_netif_new(&netif_cfg);
    if (!netif) return false;

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
    if (esp_netif_attach(netif, esp_eth_new_netif_glue(handle)) != ESP_OK ||
        esp_eth_start(handle) != ESP_OK) {
        esp_eth_driver_uninstall(handle);   // frees mac + phy
        esp_netif_destroy(netif);
        return false;
    }
    ethHandle_ = handle;
    ethNetif_ = netif;
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


// Try the stored credentials for a bounded time. Returns whether an address arrived.
bool wifiStation(uint32_t waitMs) {
    if (!ssid_[0]) return false;
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init) != ESP_OK) return false;

    wifi_config_t cfg = {};
    std::strncpy(reinterpret_cast<char*>(cfg.sta.ssid), ssid_, sizeof(cfg.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(cfg.sta.password), password_, sizeof(cfg.sta.password) - 1);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &onWifiEvent, nullptr, nullptr);
    esp_wifi_start();

    const EventBits_t bits = xEventGroupWaitBits(netEvents_, kNetGotIp, pdFALSE, pdFALSE,
                                                 pdMS_TO_TICKS(waitMs));
    if (bits & kNetGotIp) {
        // Modem power save (the default, re-armed at association) throttles receive throughput to tens of KB/s. Disabled AFTER the connection is up so nothing re-enables it; MoonBase runs for minutes on a powered board, full RX beats the milliwatts.
        esp_wifi_set_ps(WIFI_PS_NONE);
        // The transmit cap, applied as the app applies it.
        // Only a real value, and only once the connection is up, because setting it at zero or inside the radio-start stack hangs a classic ESP32.
        // The access-point fallback skips it: a hang in the recovery image outranks a possible brownout.
        if (txPowerDbm_ >= 1 && txPowerDbm_ <= 21) {
            esp_wifi_set_max_tx_power(static_cast<int8_t>(txPowerDbm_ * 4));
        }
        return true;
    }
    esp_wifi_stop();
    esp_wifi_deinit();
    return false;
}

// The last resort, and the reason SoftAP stays in the size budget: a board whose stored credentials no longer work is still reachable without a cable.
bool wifiAccessPoint() {
    esp_netif_t* ap = esp_netif_create_default_wifi_ap();
    if (!ap) return false;
    esp_netif_ip_info_t ip = {};
    ip.ip.addr = esp_ip4addr_aton(kApAddress);
    ip.gw.addr = ip.ip.addr;
    ip.netmask.addr = esp_ip4addr_aton("255.255.255.0");
    esp_netif_dhcps_stop(ap);
    esp_netif_set_ip_info(ap, &ip);
    esp_netif_dhcps_start(ap);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init) != ESP_OK) return false;
    wifi_config_t cfg = {};
    std::strncpy(reinterpret_cast<char*>(cfg.ap.ssid), kApName, sizeof(cfg.ap.ssid) - 1);
    cfg.ap.ssid_len = static_cast<uint8_t>(std::strlen(kApName));
    cfg.ap.max_connection = 2;
    cfg.ap.authmode = WIFI_AUTH_OPEN;   // an open AP: the user is standing at the device
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &cfg);
    return esp_wifi_start() == ESP_OK;
}

// ---------------------------------------------------------------------------------------------
// Installing
// ---------------------------------------------------------------------------------------------

// The one page this image serves: inline and tiny, with no filesystem read and no assets.
// The chip it was built for, spelled as the release assets spell it.
// This image is chip-specific and variant-agnostic, so that is the most it can know, and enough to filter an asset list.
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
    "href='https://moonmodules.org/projectMM/gettingstarted.html#if-your-device-shows-moonbase'>?</a></div>"
    "<p class=sub>Install firmware to return this device to normal operation."
    // WHICH MoonBase this is.
    // Filled by the boot script below rather than baked into this literal.
    // PROJECT_VER is defined only for IDF's own descriptor TU, and the descriptor is already in the image, so reading it back costs nothing and cannot drift from it.
    "<br><small id=v></small></p>"
    "<section><b>From a file</b><br><input type=file id=f accept=.bin>"
    "<button onclick=up()>Install</button>"
    // The last resort when no URL is at hand: name where the firmware-<variant>-v*.bin files live. A plain link, so it works from any device that can reach the internet.
    "<br><small>Firmware files: <a href='https://github.com/MoonModules/projectMM/releases' "
    "target=_blank rel=noopener>github.com/MoonModules/projectMM/releases</a> "
    "(the firmware-...bin matching this board)</small></section>"
    // Installing from a release without typing a URL.
    // The browser fetches the release list itself, so this image gains no network code and still only receives a URL.
    // A device in recovery most needs an easy install and can least offer the application's own picker.
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
    // The status already carries the byte counts, so reading the fraction out of it keeps one source of truth.
    // This is the third reader of that shape and the one that cannot be shared, since this image shares no sources with the app.
    // Keep both sides in step by hand.
    "const m=/(\\d+) of (\\d+)/.exec(t),b=document.getElementById('p');"
    "if(m&&+m[2]>0){b.style.display='';b.value=100*m[1]/m[2];}else{b.style.display='none';}};"
    // Surface the last install status on load: after a failed unattended install the user lands here, and the page should say what went wrong rather than look freshly booted.
    "fetch('/moonbase').then(r=>r.text()).then(t=>{if(t&&t!='idle'){S(t);"
    // AND WATCH IT.
    // An install staged by the app runs unattended, so nothing on this page had started the watcher.
    // The install finished, the app came back at this same address, and the page sat on its last status until someone reloaded by hand.
    "if(/downloading|starting|preparing|retrying/.test(t))W();}}).catch(()=>{});"
    // A device that cannot say which MoonBase it runs cannot be diagnosed: two boards looked identical while one could not install firmware, and telling them apart took a git bisect.
    "fetch('/api/version').then(r=>r.text()).then(t=>{"
    "document.getElementById('v').textContent='version '+t}).catch(()=>{});"
    // The file is sent as the RAW request body, not multipart: the device then writes bytes straight to flash with no boundary parsing, which is a meaningful saving in an image this size and matches how the application's own upload route works.
    "async function up(){const f=document.getElementById('f').files[0];if(!f)return;"
    "S('installing '+(f.size/1024|0)+' KB...');"
    "const r=await fetch('/api/firmware/upload',{method:'POST',body:f});"
    "S(await r.text());}"
    // The install runs on its own task (202); W() watches its status until the app answers (404 on /moonbase means the new firmware is up, at this same address).
    "function W(){const t=setInterval(async()=>{try{const p=await fetch('/moonbase');"
    "if(p.status==404){clearInterval(t);S('done, the app is starting...');"
    "setTimeout(()=>location.reload(),3000);}else{S(await p.text());}}"
    "catch(_){S('restarting...');}},2000);}"
    // The release list, filtered to assets this CHIP can run.
    // MoonBase is chip-specific and variant-agnostic (one image serves every variant of a chip), so it cannot know which variant the board runs.
    // It offers the ones that fit and lets the user pick, which is the same choice the application's picker presents.
    "const CHIP='" MOONBASE_CHIP "';let RELS=[],VAR='';"
    // The board's own variant, when the application has run here and persisted it.
    // With it the list is the ONE firmware this board takes, as the application's picker shows.
    // Without it, every firmware for the chip, because guessing which of three flash layouts a board has is how a user in recovery installs the wrong one.
    "fetch('/api/variant').then(r=>r.text()).then(t=>{VAR=t.trim();fillFw();}).catch(()=>{});"
    "function fwList(i){const r=RELS[i];if(!r)return [];"
    "return (r.assets||[]).map(a=>a.name).filter(n=>/^firmware-.+\\.bin$/.test(n)"
    "&&!/-(bootloader|partition-table|ota-data|slot0)\\.bin$/.test(n)"
    // The chip must match to a boundary: one target name is a prefix of another's, so a plain prefix test offered the wrong silicon's image.
    // Most assets spell the chip then a hyphen.
    // The P4 is the exception, carrying its silicon revision in the same token.
    // Requiring the hyphen alone left it with an empty list, so a revision suffix is a boundary too.
    "&&n.slice(9).startsWith(CHIP)"
    "&&(n.slice(9+CHIP.length).startsWith('-')||/^rev\\d/.test(n.slice(9+CHIP.length)))"
    "&&(!VAR||n.slice(9).startsWith(VAR+'-')));}"
    "function fillFw(){const f=document.getElementById('fw');f.innerHTML='';"
    "const l=fwList(document.getElementById('rel').selectedIndex);"
    "for(const n of l){const o=document.createElement('option');o.textContent=n;f.appendChild(o);}"
    "document.getElementById('rs').textContent=l.length?'':'no firmware for this chip in that release';}"
    "fetch('https://api.github.com/repos/MoonModules/projectMM/releases?per_page=10')"
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
    // RELOAD WHEN THE APP ANSWERS, not after a fixed wait. Eight seconds was a guess that an S3-Zero misses, so the page reloaded while the device was still booting and showed a failed page the user then had to refresh by hand. /moonbase is the identity probe: MoonBase answers it and the app 404s it, so a 404 means the application is up and serving.
    "async function ba(){const r=await fetch('/api/firmware/boot-app',{method:'POST'});S(await r.text());"
    "if(!r.ok)return;S('booting the app...');"
    "for(let i=0;i<60;i++){await new Promise(f=>setTimeout(f,1000));"
    "try{const p=await fetch('/moonbase',{cache:'no-store'});"
    "if(p.status==404){location.reload();return;}}catch(e){}}"
    "S('the app is not answering: it may not be installed');}"
    "async function cx(){S(await (await fetch('/api/firmware/cancel',{method:'POST'})).text());}"
    "</script>";

// The application's build variant ("esp32s3-zero"), or empty when the app has never run here. Empty is the honest answer for a freshly flashed or wiped device, and the page falls back to offering every firmware for the chip rather than pretending to know.

// The application slot, which from the factory partition is never the one we run from.
// Unless this image is itself in the app slot, which happens when someone installs it as the app.
// Both partitions then hold it, every install fails with a conflict, and the device answers but cannot be recovered over the network.
// Null here lets a caller say so rather than loop.
const esp_partition_t* appPartition() {
    const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
    return (part && part == esp_ota_get_running_partition()) ? nullptr : part;
}

// Write a firmware image pulled from `url` straight into the application slot.
// This is what makes an unattended install possible: point MoonBase at a release asset and it fetches it itself.
// True while any install is writing the app slot.
// Torn reads are harmless (same display-only pattern the app uses); the guard only has to stop a SECOND install from starting.
volatile bool installing_ = false;
// Set by POST /cancel; the install loops poll it and abort cleanly back to the page. The app slot is left half-written, exactly like a power cut: MoonBase stays the boot target until a later install completes.
volatile bool cancelRequested_ = false;

bool installFromUrl(const char* url) {
    esp_http_client_config_t http = {};
    http.url = url;
    http.timeout_ms = 20000;
    http.keep_alive_enable = true;
    http.crt_bundle_attach = esp_crt_bundle_attach;   // GitHub and friends are HTTPS
    // A GitHub release asset 302-redirects to a signed URL whose Location header (plus a multi-KB content-security-policy on the redirect response) overflows the client's default 512-byte header buffer, failing the connection AFTER a clean TLS handshake. Same values as the app's http_fetch_to_ota (platform_esp32_ota.cpp).
    http.disable_auto_redirect = false;
    http.max_redirection_count = 10;
    // Large receive chunks: fewer, larger flash writes per loop. Measured on the bench (classic ESP32, 40 MHz DIO flash): 4 KB chunks stream at ~46 KB/s, 16 KB at ~86, 32 KB roughly the same as 16 (write-bound from there); RAM is plentiful here.
    http.buffer_size = 32768;
    http.buffer_size_tx = 4096;
    esp_https_ota_config_t ota = {};
    ota.http_config = &http;
    // One bulk erase of the whole slot up front instead of a sector erase inlined with every 4 KB write.
    // Per-sector erases dominated the install at ~25 KB/s (identical over TLS and plain HTTP, so the wire was never the limit).
    // The upfront erase costs a few seconds, "preparing the install" covers it.
    ota.bulk_flash_erase = true;

    esp_https_ota_handle_t handle = nullptr;
    esp_err_t beginErr = esp_https_ota_begin(&ota, &handle);
    if (beginErr != ESP_OK) {
        // Numeric on purpose: the error-name table is compiled out for size (ESP_ERR_TO_NAME_LOOKUP=n), so esp_err_to_name would say "UNKNOWN ERROR".
        std::snprintf(status_, sizeof(status_), "error: cannot start the download (0x%x)",
                      static_cast<unsigned>(beginErr));
        return false;
    }
    // Not another recovery image: this writes the app slot, and one landing there leaves both partitions holding it, after which every install fails with a conflict.
    // The device still answers and still serves this page, which is what makes it easy to do and hard to undo.
    // Only a cable gets it back, and the two images sit one paste apart on the releases page.
    esp_app_desc_t incoming = {};
    if (esp_https_ota_get_img_desc(handle, &incoming) == ESP_OK &&
        std::strncmp(incoming.project_name, "projectMM-moonbase",
                     sizeof(incoming.project_name)) == 0) {
        std::snprintf(status_, sizeof(status_), "error: that is a MoonBase image, not an app");
        esp_https_ota_abort(handle);
        return false;
    }

    esp_err_t err;
    while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        if (cancelRequested_) {
            esp_https_ota_abort(handle);
            std::snprintf(status_, sizeof(status_), "canceled");
            return false;
        }
        std::snprintf(status_, sizeof(status_), "downloading: %d of %d bytes",
                      esp_https_ota_get_image_len_read(handle),
                      esp_https_ota_get_image_size(handle));
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


// The HTTP server, hand-written on raw sockets rather than the framework component: this image serves one page and receives one file, and the component would cost more than the handlers. One connection at a time is the right model, installing firmware being exclusive by nature.

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
    // The same refusal as the URL path, from the bytes in hand, its offsets shared with the app's own install path rather than copied.
    // Refused when the prefix is too short to identify rather than passed.
    // A short buffer reports no description for an image that has one, so a guard keyed on that would wave it through whenever the headers arrived alone.
    if (prefixLen < mm::firmware::kIdentifyBytes) {
        esp_ota_abort(handle);
        std::snprintf(status_, sizeof(status_), "error: could not identify the image");
        return false;
    }
    const auto incomingUp = mm::firmware::identify(
        reinterpret_cast<const uint8_t*>(prefix), prefixLen);
    if (incomingUp.described &&
        std::strcmp(incomingUp.project, "projectMM-moonbase") == 0) {
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

// The staged-URL install, off the main task, which serves meanwhile.
// A connect attempted straight after an address arrives can fail where the same one succeeds seconds later, the network still warming up around a fresh association.
// A short retry absorbs it.
char stagedUrlTask_[256];

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
        if (installFromUrl(stagedUrlTask_)) esp_restart();   // straight back into the new app
        // A failed attempt leaves its error in status_; while retries remain that error is TRANSIENT, and a watcher treating "error:" as terminal (the app's overlay does) must not see it. The final attempt's error stays as the terminal answer.
        if (attempt < 2 && !cancelRequested_)
            std::snprintf(status_, sizeof(status_), "download failed, retrying");
    }
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
        // This image's version, from the descriptor the toolchain puts in every binary.
        // Its own route rather than an addition to the status one, whose body the app parses.
        // The app reads the same version from the partition instead, since it cannot ask an image that is not running: two readers for two situations.
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

    // The cascade: Ethernet where the config wires it, then the stored credentials, then an open access point, so a board is never unreachable because its credentials went stale.

    // The unattended handoff: the app may have staged an install URL before rebooting here.
    // Read and erase it unconditionally, before anything can fail.
    // A URL that fails can never boot-loop the device and a stale one can never survive to hijack a later visit.
    char stagedUrl[256] = {};
    {
        nvs_handle_t h;
        if (nvs_open("moonbase", NVS_READWRITE, &h) == ESP_OK) {
            size_t len = sizeof(stagedUrl);
            if (nvs_get_str(h, "url", stagedUrl, &len) != ESP_OK) stagedUrl[0] = '\0';
            nvs_erase_key(h, "url");
            nvs_commit(h);
            nvs_close(h);
        }
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

    // ONE interface at a time, in the app's own preference order (eth where configured, else WiFi, else the AP): the app runs a single interface. The browser is on that interface's address, and mirroring the preference is what keeps the address valid across the handoff without a second lease to confuse anyone.
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
    if (!online) online = wifiStation(20000);

    // STA only: on the fallback AP the URL's network is not reachable, and a user is present. The install runs on its OWN task so the main task serves throughout: GET /moonbase then reports "downloading: N of M bytes" live, which is what the app's update overlay renders as a progress bar. 12 KB stack for the same reason as the main task: the TLS handshake.
    if (online && stagedUrl[0]) {
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
