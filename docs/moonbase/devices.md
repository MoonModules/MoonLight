# Devices module

<img width="320" height="441" src="../devices.png" />

Shows all MoonLight and WLED devices discovered on your local network. MoonLight devices can also be controlled from this view.

## Device table columns

* **Name** — hostname of the device (set in [WiFi station](https://moonmodules.org/MoonLight/network/sta/))
    * Click the name to open the device via mDNS
    * Devices with matching hostname prefixes are treated as a group — see [Group sync](#group-sync) below
* Light controls — mirror the values from the [Light controls module](../moonlight/lightscontrol.md); editing them sends the change to that device
    * **Lights On** — turn the device on or off
    * **Brightness** — brightness level
    * **Palette** — active palette
    * **Preset** — selected preset
* **Last Sync** — how long ago a status update was received (MoonLight devices update every 10 s or immediately on a control change; WLED devices update every 30 s or on a sync)
* **IP** — click to open the device web UI by IP address
* **Version** — firmware version (see [releases](https://github.com/MoonModules/MoonLight/releases))
* **Build** — build date of the installed firmware
* **Uptime** — time since last boot
* **Package size** — size of the last received UDP packet; useful for diagnosing protocol mismatches

!!! note "WLED devices show limited information"
    WLED's discovery packet does not include brightness, palette, or preset — those appear as 0. Name, IP, and lights-on state are available. Full control of WLED devices from this panel is not supported.

---

## Group sync

Devices are automatically grouped by their hostname. Use hyphens to define a group:

| Hostnames | Group |
|-----------|-------|
| `kitchen-1`, `kitchen-2`, `kitchen-3` | `kitchen` |
| `stage-left-1`, `stage-left-2` | `stage-left` |
| `moonlight` (no hyphen) | no group — stands alone |

Set the hostname in [WiFi → Station](https://moonmodules.org/MoonLight/network/sta/).

**How it works:**

1. Every device broadcasts its status (including light control values) every 10 seconds.
2. When a light control value changes on any device, that device immediately broadcasts the update.
3. Devices that belong to the same group apply received updates to their own lights.
4. Updates propagate without looping — a device that applies a group update does not re-broadcast it as a control change.

This means changing brightness on one group member will change it on all members within one broadcast cycle.

---

## WLED coexistence

MoonLight and WLED can share the same network without interfering with each other:

* **Discovery (port 65506)** is shared. MoonLight broadcasts its status in a WLED-compatible format, so both firmware types appear in each other's device lists.
* **Control (port 65507)** is MoonLight-only. WLED does not listen on this port, so MoonLight control commands never affect WLED devices unintentionally.

---

## Planned features

* Synchronized clocks across devices
* Distributed effects (Part 3)
* Full readback of WLED brightness/palette/preset via the WLED sync port

For developer details on the UDP protocol and packet formats, see [develop/devices](https://moonmodules.org/MoonLight/develop/devices/).

Note: This module's functionality will also be available in [ESP32 Devices](https://github.com/ewowi/ESP32Devices), a macOS and Windows companion application. 🚧
