/// QNEthernet's external-driver hook, which its driver_select.h takes when this header exists. One firmware carries the board's own Ethernet and a W5500, and qnethernet_dual_driver.cpp picks the one the board has at boot.
#pragma once

// Both built-in drivers declare these, at the same values.
#define MTU           1500
#define MAX_FRAME_LEN 1518  /* without the 4-byte FCS */

// lwIP's default options, which both drivers support: no frame pad, and checksums in software, since the W5500 computes none.
