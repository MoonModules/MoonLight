# WiFi Station

<img width="320" src="../../media/network/MB-WiFi1.png" />

* **WiFi TX Power** 🌙: WiFi transmit power can be set to optimize WiFi behavior. Some boards are known to work better by setting transmit power to 8.5 dBM (So called LOLIN_WIFI_FIX).
    * Default is typically 20 dBm (100mW) - the maximum allowed
    * Common recommended settings:
	    * For most indoor applications: 15-17 dBm
	    * For battery-powered devices: 10-15 dBm
	    * For close-range applications: 8-12 dBm
    * 🚨: When the board is in AP mode, it is set to 8.5dBM as most boards work okay with this. WiFi is also initially set to 8.5dBM

## MoonLight Analytics

MoonLight sends **anonymous** usage data to Google Analytics when it restarts.

You can **opt out** at any time by disabling Track analytics in the settings.

The data collected consists of:

* Country – e.g. France, determined via ip-api.com; only the country field is extracted.
* Firmware – e.g. esp32-s3-n16r8v
* Board model – e.g. QuinLED DigQuad, based on board presets (not yet implemented)
* MoonLight version – e.g. 0.7.0

No personal data, IP addresses, or identifiers that can trace back to an individual are collected, stored, or shared.
Each event is sent with a random, anonymous client ID, so usage cannot be linked to a specific device or person.

We use this anonymous data to understand how MoonLight is being used, identify which hardware and versions are most common, and prioritize future development and improvements accordingly.

The data is sent only to Google Analytics, which processes it to provide aggregated usage insights.

This data is fully anonymous and handled in a way that meets the principles of the GDPR and other global privacy standards.