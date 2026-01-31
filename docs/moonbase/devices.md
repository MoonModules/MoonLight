# Devices module

<img width="393" height="575" src="https://github.com/user-attachments/assets/b1de393a-d887-4b1d-b237-65f250ead279" />

Shows which other MoonLight devices are present in your local network: devices overview:

* Device name: name of this device (set the name in the [WiFi station module](https://moonmodules.org/MoonLight/network/sta/))
* Devices: Devices found on the network
  * Click on the name to go to the device via mDNS
  * Click on IP to go to the device via its IP address

## Supersync

🆕 in 0.8.1

The Devices module will also implement Supersync ©. An approach to sync multiple MoonLight devices in the network.

Part 1:

* Every device broadcasts a message every 10 seconds containing general information and the values in the [Control Module](../../moonlight/lightscontrol). (loop10s())
* If a device update one of the values of the Control Module it broadcasts the updated values. (addUpdateHandler())
* If one of the controls is changed in the devices overview, it sends a message to that device updating the controls accordingly (onUpdate())
* Every module receives these messages and update this in the devices overview (loop20ms()/receiveUDP())

Part 2:

* Devices can be grouped. Grouping is done via the hostname (see [WiFi](../../network/STA)). Using dots to group them. E.g. x.y.z1 belongs to the same group as x.y.z2.
* If a message is received of a device within it's group, the device will also update it's own controls. (receiveUDP()/updateDevices())

Note: The functionality of this module will also be available in [ESP32 Devices](https://github.com/ewowi/ESP32Devices). ESP32 Devices is a MacOS and Windows application.  🚧

Part 3 (to be done):

* Synchronized clocks
* Distributed effects