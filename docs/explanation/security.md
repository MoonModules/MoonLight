---
title: Security
---

# Security

MoonLight trusts its local network. Every client that reaches a device can read and change everything on it, the way a light switch obeys every hand in the room. The guards described below sit inside that model, and the last section says how to keep an installation safe with it.

## The trust boundary is the network

A device serves one plain HTTP port: 80 on a board, 8080 on a desktop. The REST API, the `/ws` state socket and the `/wsp` preview socket all live there, and every route answers every client. A client can set any control, add or delete modules, read and write files, install firmware, reboot the device and reset it to factory settings. The routes: [the REST API](../reference/api.md).

Every response carries `Access-Control-Allow-Origin: *`, so a web page on another origin can call the API too. That lets the [web installer](architecture/mooninstaller.md) and MoonDeck talk to devices. It also lets any page open in a browser on the same network do the same.

The UDP listeners follow the same model and accept any sender:

| Listener | Port | Open while |
|---|---|---|
| Device presence | 65506 and multicast 239.255.77.77 | the network is up |
| Art-Net, E1.31, DDP | 6454, 5568, 4048 | a Network Receive effect exists |
| OSC | 9000 | OSC is switched on (off by default) |
| RTP-MIDI | 5004, 5005 | the MIDI source is the network |
| Audio sync | 11988 | audio sync receives |
| RTSP (TCP) | 554 | an RTSP driver exists |
| Captive DNS | 53 | the access point runs |

The MQTT client connects out to the broker the user names, and the commands it receives drive controls and firmware updates like the API does. Its trust is the broker's: a broker with accounts and TLS guards the device, an open one opens it.

## Transport

The device serves plain HTTP. Its own connections out use HTTPS wherever the platform has TLS, checked against the ESP-IDF root certificate bundle with no option to skip the check. A board without TLS, such as the Teensy, hands those requests to the browser showing its page. MoonCloud goes through the [browser relay](mooncloud.md), and a firmware URL becomes a browser download.

## Secrets

Passwords live in plain text in the device's `/.config` files, where the firmware needs them to join a network. The API sends them XOR-ed and base64-encoded, which keeps them from being read at a glance and is no protection against anyone who looks: [the UI spec](../moonmodules/core/ui.md).

A client on the device's own access point (subnet 4.3.2.0/24) sees every password empty, and its requests for `/.config` files are refused. While such a client is connected, every client gets the state with passwords empty. This keeps the home network's password from a visitor who joins the access point to set the device up.

The access point uses the first known WiFi network's password, so it is as private as that network. The rule, the `opens` setting and the power-cycle gesture that opens it for one boot without a password: [the access point](../moonmodules/core/system.md#access-point).

A [backup](../reference/backup-and-restore.md) holds every file, `/.config` included, so it carries the WiFi passwords in plain text. A state document and a [preset](../reference/presets.md#what-a-preset-holds) carry no secret, and applying one keeps the stored secrets.

## Firmware

Firmware images are unsigned, and secure boot and flash encryption are off. Each image is vetted before it is written: an ESP32 image must carry the right magic byte, chip id and project name, and its checksum must match. A Teensy image must carry a valid boot header and the mark of the variant the board runs. These checks stop a wrong or damaged image, which is what they are for; anyone on the network can still install any MoonLight build.

[MoonBase](architecture/moonbase.md), the recovery image in the factory slot, serves its own unauthenticated update page on port 80.

## Files and scripts

File paths are decoded first, then refused if they contain `..`, so every path stays inside the device's file system. A mount root cannot be deleted.

[MoonLive](architecture/moonlive.md) scripts compile to native code. To run it, the ESP32 variants switch memory protection off, so the chip lets a page be written and then executed. A script calls only the builtins the firmware registers, its array reads and writes are clamped to the array, and a call-depth limit stops deep recursion. A script that loops forever is ended by the 12-second task watchdog, and two failed boots in a row start safe mode with scripts off. A script is a file, so anyone who can write files can install one.

## Provisioning and recovery

[Improv](../moonmodules/core/system.md#improv-provisioning) over USB serial sets the WiFi credentials and applies a state document. It needs physical access to the USB port, and it trusts whoever has it. Bluetooth is compiled out.

Safe mode, factory reset and the access point's one-boot open gesture are recovery paths for the person holding the device. The [troubleshooting page](../reference/troubleshooting.md) shows each.

## What leaves the device

Nothing leaves the device until the user switches a [MoonCloud](mooncloud.md) member on, each with its own consent. What each member sends and what the server keeps: [privacy policy](../legal/privacy-policy.md). MoonTalk's public message board has no accounts, so a message is as public as its text. The browser's own requests, such as the update check and the help, are listed in the same policy.

## Source

MoonLight is GPL-3.0 and its whole source is public, so anyone can audit every line described here. The UI links to it from the side nav.

## Reporting a vulnerability

Report it privately, as [SECURITY.md](https://github.com/MoonModules/MoonLight/blob/main/SECURITY.md) describes, so it is fixed before it is public.

## Securing an installation

- Put devices on a network only trusted people and devices reach: a separate WiFi network or VLAN for lighting is the standard setup.
- Keep devices off the internet: no port forward to port 80, and a firewall rule that keeps the lighting network's devices from being reached from outside.
- Leave OSC off unless a controller needs it.
- Use an MQTT broker with accounts, and TLS if it sits outside the lighting network.
- Treat a backup file like the WiFi password it contains.
- Give a show network a WiFi password of 8 to 63 characters, so the device's own access point is protected by it too.
