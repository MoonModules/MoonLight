# Plan: Ethernet, WiFi and access point as Network submodules

A device remembers several WiFi networks, picks the best one in range, and shows what it can see, joined the way a phone joins one: tap a network, type the password, connected. Each interface owns its own settings, static addressing included, and the device's own access point gets settings of its own. The Network card keeps only what spans them: the connection cascade, its status and mDNS.

## Why

- **One network only.** The Network card holds one `ssid` and `password`, so a device that moves between a workshop and a venue is reconfigured every time.
- **No view of what is in range.** Improv's code already scans, but only during provisioning; nothing tells a user which networks the device can see.
- **Static addressing is one global set.** `addressing`, `ip`, `gateway`, `subnet` and `dns` apply to whichever interface comes up, so a board that is static on its wired network is static on every WiFi network too.
- **The access point has no settings.** It is open, named after the device, always at 4.3.2.1, and starts only as a fallback.
- **The Network card mixes four concerns.** The cascade, sixteen Ethernet controls, the WiFi credentials and the hidden access-point behavior share one card.

## The standard behavior

Phones and wpa_supplicant keep a list of known networks with a priority. On boot, or after losing the connection, they scan, pick the known network in range with the highest priority and the best signal, try it, and fall through to the next on failure. Joining is connect-first: tapping a network asks for its password, connects at once, says "incorrect password" on failure, and saves the network only once it joined. iOS, Android and NetworkManager keep IP settings **per network**: each saved WiFi network and the wired interface carry their own DHCP or static settings.

## The shape

```text
Network        mode, status, mDNS: the cascade Ethernet, then WiFi, then the access point; its children in that order
├── Ethernet   board, type, pins, link-local fallback, and its own IP settings
├── WiFi       known networks, available networks, radio settings
├── Access point  when it opens, its password, channel, the clients on it
├── Improv
├── MQTT
└── Devices
```

### Ethernet

Everything Ethernet on the Network card today moves here unchanged: `ethBoard`, `ethType`, the pins, the desktop preview of those controls, and the RFC 3927 link-local fallback. It gains its own `ipSettings` (DHCP or Static) with `ip`, `gateway`, `subnet` and `dns`, the words Android, routers and WLED use.

### WiFi

- **Available networks**, the scan: SSID, signal as bars and a lock on secured networks, as in every WiFi picker. The connected network carries a check mark and the known ones are marked. **Tapping one connects**: it asks for the password inline (with "show password"), joins at once, through the live-change warning when the device is already connected, and shows the result. A network that does not join is not saved, and "incorrect password" says why.
- **Known networks**, in priority order like macOS's list: moving a row up prefers it. Each row shows its SSID, the check mark and current IP when connected, and "open" when it has no password. Its detail view holds the password and its own `ipSettings` with the static fields. Each row offers **Connect** and **Forget**, the words iOS, Android and Windows use; Forget is the row delete, and there is no separate auto-join switch.
- **`scan`**, a button, and the list says when it last scanned ("scanned 2 min ago"), since the device does not scan while connected (below); one line on the card says why.
- **Radio settings** move here from Network, below the lists as routers place them: `rssi` and `txPower` as numbers, and `txPowerSetting`.

The regulatory country stays ESP-IDF's: its default policy adopts the country of the router it joins, as a phone does, so no user is asked for one. The bench confirms a channel 13 router is found.

### Access point

The device's own network, which today starts only when nothing else connects, open and named after the device. WLED and most connected devices make it configurable the same way:

- **`opens`**: `on failure` (the default), `always`, or `never (not recommended)`, WLED's wording. "Never" is refused while neither Ethernet nor a known WiFi network is configured, since the device would then be unreachable.
- **`password`**: WPA2's 8 to 63 characters, refused at entry otherwise, or empty for an open access point. Open is the default, and the card says nothing about it, so a fresh board can be reached.
- **`clients`**, read-only: how many devices are on it.
- **`channel`**: 1 to 13, where the platform hard-codes 1 today.
- **`hidden`**: whether the access point broadcasts its name.
- Joining it opens the UI (below), with no switch for it, as in WLED, Tasmota and ESPHome.
- The name stays the device name and the address stays 4.3.2.1.

#### From the access point to the home network

First setup ends on another network, and the phone must be able to follow. After a join started from the access point, the page shows the device's new address and `.local` name with a link to each, and the access point stays up for two minutes before the fallback rule closes it, so the phone has time to read the result. The station and the access point share the radio (ESP-IDF's APSTA mode), so the access point no longer drops while the station tries a network. Joining moves the radio to the router's channel, which knocks every phone off, and an iPhone then rejoins its own network, so the hold ignores who is on the access point, and the network's row shows the `.local` link before Connect. The iPhone's sign-in screen has no address bar, so the page also says to open the link in the browser.

#### Captive portal

A phone joining a network probes fixed addresses (`captive.apple.com/hotspot-detect.html`, `connectivitycheck.gstatic.com/generate_204`) and shows a sign-in screen when the answer is not the expected one. Two small pieces make that screen the MoonLight UI:

- **A DNS responder** on UDP port 53, answering every A query with 4.3.2.1 and any other type with an empty answer, written from RFC 1035 as a pure function (query in, reply out) so the desktop tests it. Its socket exists only while the access point runs, drained without blocking from the 20 ms tick, so it costs nothing otherwise.
- **An HTTP redirect**: a request that arrived through the access point (its local address is 4.3.2.1) for a host other than 4.3.2.1 gets `302` to `http://4.3.2.1/?open=WiFi`, the card where setup continues, so a device also reached through its home network by a router's DNS name is never redirected.
- The access point's DHCP server must hand out 4.3.2.1 as the DNS server; ESP-IDF is believed to by default, which the bench confirms.

Its cost: about 1 to 2 KB of flash, one UDP socket and a 512-byte reply buffer while the access point runs, and nothing on the render path otherwise.

### Choosing a network

1. On boot, and when the connection drops, try the known networks in list order, each with the cascade's per-attempt timeout. A hidden network joins like any other, since ESP-IDF joins one given its name.
2. Nothing joins: the cascade moves on to the fallback access point, as before, and retries the known networks every minute while no phone is on it.

Roaming between access points of one network while connected is a later step; the device stays on the network it joined until it drops.

### Scanning safely

A scan takes the radio off-channel for two to five seconds, which drops incoming Art-Net and shows on a large rig (Improv refuses to scan while connected for this reason). So the device scans when its access point opens and when the user presses `scan`; never on a timer while connected. A user-started scan while connected says on the status line that output may stutter for a few seconds.

### Static addressing

Absorbs the backlog item "Static IP on WiFi STA", whose answers become the rules here for `ipSettings`:

- **Applied on connect**, and live when the settings of the network the device is on change.
- **An all-zero address is ignored** by the platform, which keeps the lease.

### Passwords

A row's password travels to the UI in the clear, over the API and the WebSocket, and shows behind the eye. That is accepted: the UI has no login, and the device is reached only on its own network.

## Platform

- `wifiScanStart()` and `wifiScanResults(results, max)`: the scan runs without blocking (`esp_wifi_scan_start` with block off) and its results are collected once the scan-done event arrives, the second call saying "not ready" until then. The cascade runs on the render thread's slow tick, where a blocking scan of two to five seconds would freeze the LEDs. The ESP32 code moves from the Improv provisioning file, which then calls it.
- The WiFi module owns the joining sequence: scan, order the candidates, try each with the per-attempt timeout, and report "none joined". Network's cascade asks it to begin and checks on it each tick, and keeps deciding between Ethernet, WiFi and the access point.
- `wifiStaInit` takes the chosen network's IP settings, applying static ones on connect.
- The desktop has no radio. As for Ethernet today, it previews the WiFi controls in developer mode, with a test hook standing in for a scan result, so the selection logic and the UI are proven on the desktop first.

## Migration

- `ssid` and `password` on Network become the first known network; Network's addressing becomes that row's and Ethernet's.
- The Ethernet controls keep their names on the new submodule.
- `migrate.js` maps both for a backup restore; an in-place update moves them once at the first boot (`NetworkModule::adoptLegacySettings`, temporary, with a backlog entry for its removal).
- Improv provisioning adds or updates a known-network row rather than writing Network's fields.

## Steps

1. ✅ **Ethernet submodule.** Move the Ethernet controls and the link-local fallback; give it its own addressing. Tests: the presets, pins and fallback tests move with it; static addressing applies on Ethernet only.
2. ✅ **WiFi submodule with one known network.** Move `ssid`, `password` and the radio settings, as a one-row list. Improv writes the row. Migration and its tests.
3. ✅ **Several known networks and the choosing order.** Unit tests on the order: list order, the next tried on failure, the fallback access point when none joins. Connect and Forget on a known row.
4. ✅ **Available networks and connect-first joining.** The platform scan moves out of Improv; bars, lock, check mark and "scanned ago"; tap, password, join now, save only on success, "incorrect password" on failure.
5. ✅ **Per-network IP settings**, applied on join and live. 🚧 Validating before use and warning before a live change are not built (below).
6. ✅ **Access point submodule:** `opens`, `password`, `clients`, `channel` and `hidden`, with the refusal of "never" when nothing else is configured. Tests on the cascade with each `opens` value.
7. ✅ **Captive portal and the handoff:** the DNS reply builder with its unit tests (an A query, another type, a malformed and an oversized packet), the socket's lifecycle tied to the access point, the HTTP redirect with a test, and the new-address page with the access point held open.
8. **Bench**, ✅ for what an S3 shows: two known networks with one absent, first setup from a computer, a protected hidden access point, the iPhone sign-in screen. 🚧 The rest waits (below).

## Verified, and what waits for more hardware

Tested on the one board at hand, an S3 on a 2.4 GHz network, and pinned by tests that can run again:

- **Unit tests** cover the logic: the cascade and its `opens` rules, the known list and its order, connect-first joining and its failure words, per-network IP settings applied on join and live, Ethernet taking over and refusing a card join, the fallback holding off while a phone is on the access point, the DNS reply, the redirect rule, the two-minute handoff, the password lengths and the `?open=` link.
- **Live scenarios**, run only when named: `scenario_AccessPoint_first_setup_from_its_own_network` walks first setup with the computer as the phone and passed four times on the S3; `scenario_WiFi_known_networks_fall_back_in_order` and `scenario_AccessPoint_protected_hidden_and_always_on` (a password, a hidden name, opening always) passed once each.

Waiting for more devices and a cable:

- 🚧 **Memory on a board without PSRAM.** Build the classic ESP32 and the P4 with these changes, flash a board without PSRAM, and compare its free heap against main.
- 🚧 **Ethernet.** The Ethernet card on a wired board (P4, S31 or a classic Ethernet board): DHCP, a static address, and a cable plugged in while WiFi is joined.
- 🚧 **Phones.** The full first setup on an iPhone, the sign-in screen opening on the WiFi card, and on an Android phone.
- 🚧 **Networks.** A hidden network joined as a station, a channel 13 router, and static addressing on a WiFi network.
- 🚧 **`opens: never`** on hardware, with USB at hand to recover.
- 🚧 **Not built yet:** ordering the known networks by signal strength from a scan; validating static IP settings before use, beyond ignoring an all-zero address; asking before a live IP change, which drops the open page.
- ✅ **MoonBase** with the new layout, on a classic ESP32: it joins the app's WiFi, opens the device's own access point with the sign-in screen, and installs from a GitHub release. Two live scenarios pin the first two.
- 🚧 **MoonBase over Ethernet**, on an Olimex Gateway with the new scenario.
- ✅ **A staged install survives a reset in MoonBase.** It keeps the URL until an install ends and counts installs started, stopping after three that reset the device. On a D32 whose supply collapses whenever the radio starts, 3 of 3 hand-overs completed where 2 of 4 were lost before.
- 🚧 **Two unexplained S3 failures, each without a serial log at the time.** A card join that did not start in one first-setup run. A freeze, silent on serial and off the network until reset, after the fallback scenario, after which the second known network was gone from the saved file.

## Subtraction

- From Network: `ssid`, `password`, `rssi`, `txPower`, `txPowerSetting`, `addressing`, `ip`, `gateway`, `subnet`, `dns`, every Ethernet control and the hard-wired access-point behavior, leaving the cascade and its status.
- No switch for the captive portal and no country setting: the behavior users expect, with nothing to configure.
- The global static addressing that applied to whichever interface came up.
- The scan code inside Improv provisioning, which calls the platform scan instead.
- The backlog item "Static IP on WiFi STA", deleted when this ships.

## Next, found in the pre-merge review

- **The Improv scan and the radio teardown race.** `wifiScanStart()` runs on Improv's task and switches the radio mode while the render task may run `wifiRadioDown()`, which takes no lock; one mutex around the radio's mode and netifs, or the scan handed to the render task, closes it. Verified on a board.
- **One IP-settings struct.** Ethernet keeps its five address fields and its live re-apply in the child, WiFi in the parent with a signature seeded by hand on every start path; one `{mode, ip, gateway, subnet, dns}` in `IpSettings.h` with its signature and its apply serves both.
- **The cascade by its states.** `onConnected` dispatches on a string where the `State` enum exists, and the start-the-station transition and the stop-and-note pair are each copied six to eight times; one function per transition.

## Out of scope at first

The advanced settings users will ask for sooner or later are listed in the backlog's [advanced network settings](../future/backlog-core.md) item: roaming, enterprise WiFi, pinning to one access point, power save, the country code, the access point's address, and IPv6.

## Decided

- **An open network needs no confirmation**, as on a phone: it joins a saved open network like any other, and the row is marked open, a warning that the device would join any network of that name.
- **The words are the ones users know:** "Known networks" and "Available networks" (macOS, Windows), Connect and Forget (iOS, Android, Windows), "IP settings: DHCP / Static" (Android, routers, WLED), and "opens: on failure / always / never" (WLED).
- **Ethernet is preferred over WiFi** when both are up, as every operating system does by giving the wired route the lower metric. The submodules show in the cascade's order: Ethernet, WiFi, access point.
