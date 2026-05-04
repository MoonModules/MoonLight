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

```
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

```
[UDPWLEDHeader — 44 bytes]  ← WLED reads only this portion
[Char<32> versionStr    ]   ← human-readable version, e.g. "0.9.1"
[char build[16]         ]   ← build date string, e.g. "20260411"
[uint32_t uptime        ]
[uint16_t packageSize   ]   ← sizeof(UDPMessage); receiver uses for size-based dispatch
[uint8_t brightness     ]
[uint8_t palette        ]
[uint8_t preset         ]
```

Total: **101 bytes** with `__attribute__((packed))`. WLED receives all 101 bytes but only parses the first 44; the rest is ignored. MoonLight receivers recognise the full packet by size.

### `UDPControlMessage` — MoonLight-only control (port 65507)

```
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

### Unicast control — from the device table UI

When a user edits a row in the devices table, `onUpdate()` fires:

- If the target IP is **this device** → apply directly via `_moduleControl->update()`.
- If the target IP is **another device** → send `UDPControlMessage` unicast to `targetIP:65507` with `targetName` set to the receiver's hostname.

The receiver's `processControlMessage()` accepts the packet if `strcmp(myName, msg.targetName) == 0`.

After applying, the receiver re-broadcasts to its own group on port 65507 (origin `_moduleName` triggers `sendUDP(true)` in `addUpdateHandler`), propagating the change to any other devices in the same group. The re-broadcast uses empty `targetName` so other devices apply it as a group broadcast but do not re-broadcast again (origin `"group"` suppresses the next `sendUDP`).

### Group broadcast control — from the addUpdateHandler

When local light controls change (via UI or any other mechanism), `addUpdateHandler` fires `sendUDP(true)`, which:

1. Builds a `UDPControlMessage` with empty `targetName`.
2. Broadcasts to `255.255.255.255:65507`.

Receivers call `processControlMessage()`:

```cpp
bool isUnicast       = targetName != "" && targetName == myName;
bool isGroupBroadcast = targetName == "" && partOfGroup(myName, senderName);
```

Group members apply the control with origin `"group"` — this fires `addUpdateHandler` again but `sendUDP(false)` (no re-broadcast), breaking the loop.

### Loop prevention

`addUpdateHandler` receives the `originId` string:

```cpp
sendUDP(originId != "group");   // true = control broadcast, false = discovery only
```

Anything that arrives via group broadcast is applied with `originId = "group"`, so it never re-triggers a control broadcast.

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

`loop20ms()` guards on `deviceUDPConnected` before calling `receiveUDP()`. `receiveUDP()` polls both sockets regardless, so control packets are processed whenever the discovery socket is up (the control socket `parsePacket()` is a no-op if unbound).

`sendUDP(true)` guards on `deviceControlUDPConnected` — if port 65507 fails to bind, group control broadcasts are silently skipped (a `LOGW` in `loop10s` covers the bind failure).

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
