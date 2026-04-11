# Network

## WiFi

### `_connecting` guard in WiFiSettingsService

`WiFiSettingsService::manageSTA()` manages the STA reconnection loop. The ESP-IDF WiFi stack is not re-entrant — calling `WiFi.begin()` while a previous attempt is still in the IDF state machine causes silent corruption. The `_connecting` flag prevents this.

#### Flag lifecycle

| Event | Effect on `_connecting` |
|---|---|
| `connectToWiFi()` called (just before `WiFi.begin()`) | Set to `true` |
| `ARDUINO_EVENT_WIFI_STA_CONNECTED` (success path) | Set to `false`, `_lastConnectionAttempt` reset |
| `onStationModeDisconnected()` (failure / drop) | Set to `false` |
| `manageSTA()` early-return: already connected / no SSID / offline | Set to `false` |

#### Guard in manageSTA()

```cpp
if (_connecting) {
    if ((unsigned long)(millis() - _lastConnectionAttempt) < 30000UL)
        return;   // still waiting for IDF result — do not re-enter
    // 30-second escape: both CONNECTED and DISCONNECTED events missed
    _connecting = false;
}
```

`_lastConnectionAttempt` is set immediately before `manageSTA()` is called, so it reliably records when the current attempt started.

#### 30-second timeout

If the IDF event loop drops both `STA_CONNECTED` and `STA_DISCONNECTED` (rare but observed on congested stacks), `_connecting` would stay `true` permanently and block all future reconnections. The 30-second timeout in the guard above is the escape hatch: after 30 s with no event, `_connecting` is reset with a `LOGW` and the next `manageSTA()` call retries normally.

## ModuleDevices — UDP Port Segregation

**Problem:** WLED devices listen on port 65506 for metadata/discovery. When MoonLight sent control commands (brightness, palette, preset) to the same port, WLED devices received and misinterpreted them, causing interference.

**Solution:** Port segregation via two separate UDP sockets in `ModuleDevices` ([`src/MoonBase/Modules/ModuleDevices.h`](https://github.com/MoonModules/MoonLight/blob/main/src/MoonBase/Modules/ModuleDevices.h)):

| Port | Socket | Direction | Purpose | WLED visibility |
|------|--------|-----------|---------|-----------------|
| **65506** | `deviceUDP` | Bidirectional | Device discovery & metadata broadcasts; shared with WLED | ✅ Sees WLED packets, WLED sees device info |
| **65507** | `deviceControlUDP` | Outbound unicast | MoonLight-only control commands (from UI → device) | ❌ WLED cannot see |

**Implementation:**

- `loop10s()` initializes both sockets; control socket binding failures are logged.
- `onUpdate()` sends targeted device control changes via `deviceControlUDP.beginPacket(..., 65507)` instead of `deviceUDP`.
- `receiveUDP()` polls both sockets via a shared `processUDPMessage()` helper:
  - **Port 65506 (discovery):** Use existing group/broadcast logic; also process device info from WLED.
  - **Port 65507 (control):** Enforce `isControlCommand + hostname match` — only apply if message is addressed to this device.

**Trade-off:** Adds a second socket (small memory cost, ~300 bytes stack per packet). Benefit: Clean isolation; no cross-vendor interference; WLED remains compatible.
