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
