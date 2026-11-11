---
title: Security
---

# Security

MoonLight trusts its local network. Every client that reaches a device can read and change everything on it, the way a light switch obeys every hand in the room. The guards described below sit inside that model, and the last section says how to keep an installation safe with it.

## The trust boundary is the network

A device serves one plain HTTP port: 80 on a board, 8080 on a desktop. The REST API, the `/ws` state socket and the `/wsp` preview socket all live there, and every route answers every client on the network. A client can set any control, add or delete modules, read and write files, install firmware, reboot the device and reset it to factory settings. The routes: [the REST API](../reference/api.md).

A web page from outside the network is refused. A browser names the page a request comes from in its `Origin` header, and a device answers 403, the WebSocket included, when that page's host is not on the network: a private, shared (RFC 6598) or loopback address, `localhost`, a `.local`, `.home.arpa` or `.internal` name, or a name without a dot; a page the device served, called under the name it was opened by, passes as well. So a site from the internet cannot use a browser on the network against a device.

A DNS-rebinding page, which points its own name at the device and reads it as its own, is refused by the `Host` header. A device answers an IP address, a local name (`localhost`, `.local`, `.home.arpa`, `.internal`, one label) and its own name under any domain, as its router names it from the hostname the device sends (`mm-acfd.fritz.box`, `mm-acfd.lan`); the name the device already has is all the rule reads. What remains is a rebinding page that guesses the device's name and serves it under its own domain, so a name that is hard to guess, such as the default `MM-XXXX`, keeps that out of reach.

Both checks apply to requests on the network; a request through the device's own access point passes them, since its captive check asks under any name and its clients reach the device alone. Pages a device serves, the desktop app, other devices, MoonDeck, Home Assistant and scripts, which send no `Origin`, are unaffected; MoonBase applies the same rule.

The UDP listeners follow the same model and accept any sender:

| Listener | Port | Open while |
|---|---|---|
| Device presence | 65506 and multicast 239.255.77.77 | the network is up |
| Art-Net, E1.31, DDP | 6454, 5568, 4048 (`ddp_port`) | a Network Receive effect listens for the protocol |
| OSC | 9000 | OSC is switched on (off by default) |
| RTP-MIDI | 5004, 5005 | the MIDI source is the network |
| Audio sync | 11988 | audio sync receives |
| RTSP (TCP) | 554 | an RTSP driver exists |
| Captive DNS | 53 | the access point runs |

The MQTT client connects out to the broker the user names, and the commands it receives drive controls and firmware updates like the API does. Its trust is the broker's: a broker with accounts and TLS guards the device, an open one opens it.

## The Devices map

The [map](../moonmodules/core/system.md) shows what a device hears on the network: each device's name, address, MAC, firmware and version, the traffic between them and the services each offers, to anyone who reaches the device. It keeps all of it in memory and saves only the addresses a user added.

Unasked, a device sends only its presence packet every 10 seconds, two mDNS browses a minute and one probe of an added address a minute, and reads ARP from its own table.

From the map a user moves a WLED, Tasmota or ESPHome device to MoonLight through that firmware's own update page, and the map keeps that firmware's guards: Tasmota's Referer check needs the owner's `SetOption128`, an ESPHome update password or encryption means moving by cable, and a WLED update lock refuses the move. The ESPHome relay, which hands an image on from the device, connects only to addresses on the network.

Every parser of what the network sends, the presence packet, mDNS, the probed page, ESPHome's update protocol and the relay's query, is tested with cut, looping and hostile input.

The map stays on the network. A feature that carries it beyond, such as a group across homes or remote help on another user's device, crosses the trust boundary and gets its own review of what leaves and who may see or act on it.

## Transport

The device serves plain HTTP. Its own connections out use HTTPS wherever the platform has TLS, checked against the ESP-IDF root certificate bundle with no option to skip the check. A board without TLS, such as the Teensy, hands those requests to the browser showing its page. MoonCloud goes through the [browser relay](mooncloud.md), and a firmware URL becomes a browser download.

## Secrets

Passwords live in plain text in the device's `/.config` files, where the firmware needs them to join a network. The API sends them XOR-ed and base64-encoded, which keeps them from being read at a glance and is no protection against anyone who looks: [the UI spec](../moonmodules/core/ui.md).

A page from the internet cannot read them, by the rule above; anyone on the network can. A client on the device's own access point (subnet 4.3.2.0/24) sees every password empty, and its requests for `/.config` files are refused. While such a client is connected, every client gets the state with passwords empty. This keeps the home network's password from a visitor who joins the access point to set the device up.

The access point uses the first known WiFi network's password, so it is as private as that network. The rule, the `opens` setting and the power-cycle gesture that opens it for one boot without a password: [the access point](../moonmodules/core/system.md#access-point).

A [backup](../reference/backup-and-restore.md) holds every file, `/.config` included, so it carries the WiFi passwords in plain text. A state document and a [preset](../reference/presets.md#what-a-preset-holds) carry no secret, and applying one keeps the stored secrets.

## Firmware

Firmware images are unsigned, and secure boot and flash encryption are off. Each image is vetted before it is written: an ESP32 image must carry the right magic byte, chip id and project name, and its checksum must match. A Teensy image must carry a valid boot header and the mark of the variant the board runs. These checks stop a wrong or damaged image, which is what they are for; anyone on the network can still install any MoonLight build.

[MoonBase](architecture/moonbase.md), the recovery image in the factory slot, serves its own unauthenticated update page on port 80, under the same `Origin` rule, and installs only from an http or https URL. Its page and the Devices map load the web installer's picker from moonmodules.org, MoonLight's own site, as code running with the page's rights.

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
- Keep devices off the internet: no port forward to port 80, and a firewall rule that keeps the lighting network's devices from being reached from outside. The `Origin` rule stops a browser's page; a client a forwarded port lets in from the internet reaches the device as any client on the network does.
- Open a device by its address or a name beginning with its own: under another name, such as a dynamic DNS name, it answers 403.
- Leave OSC off unless a controller needs it.
- Use an MQTT broker with accounts, and TLS if it sits outside the lighting network.
- Treat a backup file like the WiFi password it contains.
- Give a show network a WiFi password of 8 to 63 characters, so the device's own access point is protected by it too.
