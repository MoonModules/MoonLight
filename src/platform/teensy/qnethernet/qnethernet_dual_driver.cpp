/// @defgroup platform_teensy_eth_driver One firmware, both Ethernet chips
/// @{
/// QNEthernet's driver contract, answered by whichever chip the board has: its own PHY, else a W5500 on the SPI port.
///
/// @moreinfo
///
/// ## Why one firmware carries both
///
/// A chip's firmware carries every Ethernet driver the chip can host, and which one a board uses is found or configured at run time, never a build choice.
/// QNEthernet compiles one driver per build, so both of its built-in drivers are compiled with their `driver` namespace renamed per file. This file is the external driver its hook takes.
///
/// ## Detection
///
/// The on-board PHY answers on its management bus, and QNEthernet's own driver already reports a 4.1-NE, which has none, as having no hardware.
/// So the PHY is asked first and the W5500 second, once, at the first call; a board with neither keeps the PHY driver, which reports no hardware.

#include "qnethernet/lwip_driver.h"
#include "qnethernet_dual_driver.h"

namespace qindesign::network {

// The two built-in drivers' copies of the contract, renamed by the build.
#define MM_QNETHERNET_DRIVER_API                                                       \
    void get_capabilities(DriverCapabilities* dc);                                      \
    bool is_unknown();                                                                  \
    void get_system_mac(uint8_t mac[ETH_HWADDR_LEN]);                                   \
    bool get_mac(uint8_t mac[ETH_HWADDR_LEN]);                                          \
    bool set_mac(const uint8_t mac[ETH_HWADDR_LEN]);                                    \
    bool has_hardware();                                                                \
    void set_chip_select_pin(int pin);                                                  \
    bool init();                                                                        \
    void deinit();                                                                      \
    struct pbuf* proc_input(struct netif* netif, int counter);                          \
    void poll(struct netif* netif);                                                     \
    void get_link_info(LinkInfo* li);                                                   \
    bool set_link(const LinkSettings* ls);                                              \
    err_t output(struct pbuf* p);                                                       \
    bool output_frame(const void* frame, size_t len);                                   \
    bool set_incoming_mac_address_allowed(const uint8_t mac[ETH_HWADDR_LEN], bool allow);
namespace driver_t41 { MM_QNETHERNET_DRIVER_API }
namespace driver_w5500 { MM_QNETHERNET_DRIVER_API }
#undef MM_QNETHERNET_DRIVER_API

namespace {
bool s_chosen = false;
bool s_w5500 = false;

bool w5500() {
    if (!s_chosen) {
        s_w5500 = !driver_t41::has_hardware() && driver_w5500::has_hardware();
        s_chosen = true;
    }
    return s_w5500;
}
}  // namespace

// Every call goes to the chosen driver.
#define MM_ONE_OF(call) (w5500() ? driver_w5500::call : driver_t41::call)

namespace driver {
void get_capabilities(DriverCapabilities* dc) { MM_ONE_OF(get_capabilities(dc)); }
bool is_unknown() { return MM_ONE_OF(is_unknown()); }
void get_system_mac(uint8_t mac[ETH_HWADDR_LEN]) { MM_ONE_OF(get_system_mac(mac)); }
bool get_mac(uint8_t mac[ETH_HWADDR_LEN]) { return MM_ONE_OF(get_mac(mac)); }
bool set_mac(const uint8_t mac[ETH_HWADDR_LEN]) { return MM_ONE_OF(set_mac(mac)); }
bool has_hardware() { return MM_ONE_OF(has_hardware()); }
// Before detection, which probes the W5500 through this pin, so both hear it.
void set_chip_select_pin(int pin) {
    driver_t41::set_chip_select_pin(pin);
    driver_w5500::set_chip_select_pin(pin);
}
bool init() { return MM_ONE_OF(init()); }
void deinit() { MM_ONE_OF(deinit()); }
struct pbuf* proc_input(struct netif* netif, int counter) { return MM_ONE_OF(proc_input(netif, counter)); }
void poll(struct netif* netif) { MM_ONE_OF(poll(netif)); }
void get_link_info(LinkInfo* li) { MM_ONE_OF(get_link_info(li)); }
bool set_link(const LinkSettings* ls) { return MM_ONE_OF(set_link(ls)); }
err_t output(struct pbuf* p) { return MM_ONE_OF(output(p)); }
bool output_frame(const void* frame, size_t len) { return MM_ONE_OF(output_frame(frame, len)); }
bool set_incoming_mac_address_allowed(const uint8_t mac[ETH_HWADDR_LEN], bool allow) {
    return MM_ONE_OF(set_incoming_mac_address_allowed(mac, allow));
}
}  // namespace driver

#undef MM_ONE_OF

}  // namespace qindesign::network

bool mmEthernetIsW5500() { return qindesign::network::w5500(); }

/// @}
