/// What the dual Ethernet driver (qnethernet_dual_driver.cpp) tells the platform.
#pragma once

/// Whether the board talks through a W5500 rather than its own PHY, found at the first driver call.
bool mmEthernetIsW5500();
