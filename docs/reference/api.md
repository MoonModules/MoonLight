---
hide:
  - navigation
  - toc
---

# REST API

Every HTTP route a device serves, with its parameters and answers, rendered from [openapi.yaml](openapi.yaml), the API's OpenAPI 3.1 description. A test holds it to the routes the server dispatches, both ways. How to use them together is in [Integrating](integrating.md).

<div id="redoc" style="background:#fff"></div>
<script src="https://cdn.jsdelivr.net/npm/redoc@2.5.4/bundles/redoc.standalone.js"></script>
<script>Redoc.init("openapi.yaml", { hideHostname: true }, document.getElementById("redoc"));</script>

## BLE control transport

The optional ESP-IDF NimBLE peripheral carries opaque newline-delimited requests independently of Wi-Fi. Enable `CONFIG_BT_NIMBLE_ENABLED`; enable `CONFIG_BT_NIMBLE_NVS_PERSIST` to retain bonds. Other builds report unavailable. The caller starts and stops the peripheral and consumes complete requests on its scheduler; this transport supplies no state dispatcher or product configuration. The service UUID is `bee30700-a2eb-4f7a-889b-13192c8c1819`; encrypted State/read+notify, encrypted Command/write and public Identity/read use suffixes `0701`, `0702` and `0703`. Pairing uses LE Secure Connections Just Works, without MITM protection. Responses contain at most 512 bytes; notifications contain little-endian uint32 revision and uptime. Identity is supplied by the caller.

Requests span ATT writes, end in LF (CRLF accepted) and have a 512-byte bound. One pending request applies backpressure. Overflow, NUL and batched lines are rejected; disconnect discards partial input. Receive and publish use try-locks so the render task never waits for a GATT callback. A new response replaces the readable result and queues a notification on the host task.
