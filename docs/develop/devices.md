# ModuleDevices — Developer Reference

Source: [`src/MoonBase/Modules/ModuleDevices.h`](https://github.com/MoonModules/MoonLight/blob/main/src/MoonBase/Modules/ModuleDevices.h)

ModuleDevices handles automatic discovery of MoonLight and WLED devices on the local network and provides group-synchronized light control across multiple devices. It uses two UDP sockets with strictly separated responsibilities.

---

## UDP Port Architecture

| Port | Socket | Direction | Who uses it |
|------|--------|-----------|-------------|
| **65506** | `deviceUDP` | Bidirectional | MoonLight **and** WLED — shared discovery port |
| **65507** | `deviceControlUDP` | Bidirectional | MoonLight only — WLED never listens here |

The key invariant: **control commands never go on port 65506**. WLED listens on 65506 for device metadata. Sending control commands on that port caused WLED to misinterpret them as malformed discovery packets, corrupting its state. Port 65507 is MoonLight-private.

---

## Packet Formats

### `UDPWLEDHeader` — 44 bytes, WLED-compatible

```text
byte 0:    token   — must be 255 (WLED validates this)
byte 1:    id      — must be 1   (WLED validates this)
byte 2–5:  ip0–ip3 — sender IP (WLED checks ip0 == localIP[0] as subnet check)
byte 6–37: name    — null-padded hostname (32 bytes)
byte 38:   type    — board type (32=ESP32, 33=S2, 34=S3, 35=C3, 36=P4) | 0x80 if lights on
byte 39:   insId   — last IP octet; WLED uses this as instance index
byte 40–43:version — numeric build date, YYYYMMDD parsed from APP_DATE
```

A `static_assert(sizeof(UDPWLEDHeader) == 44)` enforces this at build time. This layout matches `UDPWLEDMessage` in [StarLight/SysModInstances.h](https://github.com/ewowi/StarLight/blob/main/src/Sys/SysModInstances.h) and the WLED instances protocol.

### `UDPMessage` — full MoonLight discovery packet (port 65506)

```text
[UDPWLEDHeader — 44 bytes]  ← WLED reads only this portion
[char versionStr[32]    ]   ← human-readable version, e.g. "0.9.1"
[char build[16]         ]   ← build date string, e.g. "20260411"
[uint32_t uptime        ]
[uint16_t packageSize   ]   ← sizeof(UDPMessage); receiver uses for size-based dispatch
[uint8_t brightness     ]
[uint8_t palette        ]
[uint8_t preset         ]
```

Total: **101 bytes** with `__attribute__((packed))`. WLED receives all 101 bytes but only parses the first 44; the rest is ignored. MoonLight receivers recognise the full packet by size.

### `UDPControlMessage` — MoonLight-only control (port 65507)

```text
[UDPWLEDHeader — 44 bytes]  ← sender identification
[char targetName[32]     ]  ← unicast: receiver hostname; group broadcast: empty string
[uint8_t brightness      ]
[uint8_t lightsOn        ]
[uint8_t palette         ]
[uint8_t preset          ]
```

Total: **80 bytes** with `__attribute__((packed))`. WLED never sees this packet.

---

## Discovery Flow (port 65506)

`loop10s()` calls `sendUDP(false)`, which:

1. Builds a `UDPMessage` with `token=255, id=1`, correct IP bytes, board type, and current control state.
2. Broadcasts to `255.255.255.255:65506`.
3. Calls `updateDevices()` on itself (UDP does not loop back to the sender).

`receiveUDP()` dispatches incoming packets on port 65506 by **size**:

| Packet size | Interpretation | Handler |
|-------------|---------------|---------|
| `sizeof(UDPWLEDHeader)` = 44 | WLED device | `updateDevicesWLED()` — limited fields (name, ip, lightsOn only) |
| `sizeof(UDPMessage)` = 101 | MoonLight device | `updateDevices()` — all fields |
| anything else | Unknown / corrupted | Log warning, discard |

Both handlers also validate `token==255 && id==1` before accepting a packet.

### Why WLED gets limited fields

WLED's 44-byte discovery packet does not include brightness, palette, or preset — those travel on WLED's separate sync port (21324, `UDPWLEDSyncMessage`). MoonLight currently does not listen on 21324, so WLED devices show `brightness=0, palette=0, preset=0` in the device table. This could be extended in a future iteration by adding a listener on the WLED notifier port.

---

## Control Flow (port 65507)

### From the device table UI (`onUpdate()`)

When a user edits a row in the devices table, `onUpdate()` fires and dispatches on three cases:

**This device (`targetIP == activeIP`):**
Apply directly via `_moduleControl->update(..., "unicast")`. The `addUpdateHandler` then broadcasts to the group and sends a status update.

**Remote device, same group** (`partOfGroup` matches in either direction between sender and target hostnames):
Send a `UDPControlMessage` group broadcast to `255.255.255.255:65507` with **empty `targetName`**. All group members receive it in one packet and apply it via `processControlMessage()`.

**Remote device, different group:**
Send a `UDPControlMessage` unicast to `targetIP:65507` with `targetName` set to the receiver's hostname. The receiver applies the change and then re-broadcasts to its own group (see loop prevention below).

### Receiving a control packet (`processControlMessage()`)

`processControlMessage()` is called for every 80-byte packet arriving on port 65507:

```cpp
// getSystemHostname() returns String by value — stored to avoid a dangling char* pointer
String myName = esp32sveltekit.getSystemHostname();
if (myName == senderName) return;   // ignore own echo

bool isUnicast        = (targetName[0] != '\0') && (myName == targetName);
bool isGroupBroadcast = (targetName[0] == '\0') && partOfGroup(myName, senderName);
```

- `isUnicast` → apply with origin `"unicast"`.
- `isGroupBroadcast` → apply with origin `"group"`.
- Neither → discard silently.

### Group broadcast control — from `addUpdateHandler`

Any local light control change fires `addUpdateHandler`:

```cpp
if (originId != "group") sendUDP(true);  // broadcast control to group; suppressed only when change arrived via group broadcast
sendUDP(false);                          // always broadcast status so all device tables update immediately
```

`sendUDP(true)` builds a `UDPControlMessage` with empty `targetName` and broadcasts to `255.255.255.255:65507`.

### Loop prevention

| Origin when applying | Source | `sendUDP(true)` fired? |
|---|---|---|
| numeric (WebSocket client ID) | UI change on control panel | yes → group broadcast |
| `"unicast"` | received unicast from different group, or self-targeted device table edit | yes → re-broadcasts to own group |
| `"group"` | received group broadcast | **no** — loop stops here |

A device that applies a group broadcast uses origin `"group"`, suppressing `sendUDP(true)` and stopping propagation after one hop.

---

## Group Naming

`partOfGroup(base, device)` uses hostname prefix matching via hyphens:

- `base = "kitchen-1"` → prefix = `"kitchen"` → matches `"kitchen-2"`, `"kitchen-3"`, etc.
- Devices without a hyphen only match themselves exactly.

This is the sole mechanism for group membership — no configuration required beyond the hostname set in the WiFi STA module.

---

## Socket Lifecycle

Both sockets are initialised in `loop10s()` on first successful network connection:

```cpp
deviceUDPConnected        = deviceUDP.begin(65506);
deviceControlUDPConnected = deviceControlUDP.begin(65507);
```

`loop20ms()` guards on `deviceUDPConnected || deviceControlUDPConnected` before calling `receiveUDP()`. Inside `receiveUDP()`, the discovery section (port 65506) runs unconditionally, but the control section (port 65507) returns early if `deviceControlUDPConnected` is false — so a failed control socket bind does not block discovery processing.

`sendUDP(true)` guards on `deviceControlUDPConnected` — if port 65507 fails to bind, group control broadcasts are skipped with a `LOGW`.

---

## Protocol Versioning / Breaking Changes

The `packageSize` field in `UDPMessage` carries `sizeof(UDPMessage)`, and `receiveUDP()` uses exact size matching to identify packet types. This means changing the `UDPMessage` layout is a **breaking protocol change**: devices running different firmware versions will not recognise each other's discovery packets until all are updated.

Current sizes (as of the WLED-compatibility rewrite):

| Struct | Size |
|--------|------|
| `UDPWLEDHeader` | 44 bytes (static_assert enforced) |
| `UDPMessage` | 101 bytes |
| `UDPControlMessage` | 80 bytes |

Previous `UDPMessage` (before WLED-compatibility rewrite) was **97 bytes**. Mixed networks with old and new firmware lose MoonLight-to-MoonLight discovery until all devices are updated. WLED compatibility was broken in the old format (`token=0` caused WLED to reject packets immediately).

---

## Related

- End-user docs: [moonbase/devices](https://moonmodules.org/MoonLight/moonbase/devices/)
- Upstream reference: [StarLight SysModInstances.h](https://github.com/ewowi/StarLight/blob/main/src/Sys/SysModInstances.h)
- WLED instance protocol: `UDPWLEDMessage` (44 bytes, port 65506) documented in StarLight source
- Group sync behaviour: [WiFi STA — hostname](https://moonmodules.org/MoonLight/network/sta/)
