# Devices module

<img width="320" height="441" src="../devices.png" />

Overview of MoonLight (and WLED) devices in your local network. MoonLight devices can also be controlled.

* Devices:
    * **Name**: name of this device (set the name in the [WiFi station module](https://moonmodules.org/MoonLight/network/sta/))
        * Click on the name to go to the device via mDNS
        * Devices are grouped if the name contains hyphens, see below.
    * Light controls: change values found in the [Light controls module](../../moonlight/lightscontrol/)
        * **Lights On**: set the device on or off
        * **Brightness**: control the brightness of a device
        * **Palette**: Palette used
        * **Preset**: Selected preset
    * **Last Sync**: how long ago a device update has been received (normally every 10s for a MoonLight device or when a control updated, WLED every 30s)
    * **IP**: click on the IP to go to the device via its IP address
    * **Version**: Firmware version installed (see [releases](https://github.com/MoonModules/MoonLight/releases))
    * **Build**: Date of the installed firmware
    * **Uptime**: How long the device is running
    * **Package** size: For debugging

## Supersync

🆕 in 0.8.1

The Devices module will also implement Supersync. An approach to sync multiple MoonLight devices in the network.

Part 1:

* Every device broadcasts a message every 10 seconds containing general information and the values in the [Control Module](../../moonlight/lightscontrol).
* If one of the values in the Control Module is updated, it broadcasts the updated values.
* If one of the controls is changed for a device in the devices overview, it sends a message to that device updating to update its controls
* Every module receives these messages and updates them in the devices overview


Part 2:

* Devices can be grouped via the hostname (see [WiFi](../../network/sta/)). Using hyphens to group them. E.g. x-y-z1 belongs to the same group as x-y-z2.
* If a message is received from a device within its group, the device will also update its own controls.
* It will notify other devices on its update, but the message is not a control message to avoid infinite sending loops

Part 3 (to be done):

* Synchronized clocks
* Distributed effects

Note: The functionality of this module will also be available in [ESP32 Devices](https://github.com/ewowi/ESP32Devices). ESP32 Devices is a MacOS and Windows application.  🚧