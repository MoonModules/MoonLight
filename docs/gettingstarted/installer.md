# MoonLight Installer

This page describes how to install <a href="https://github.com/MoonModules/MoonLight" target="_blank">MoonModules/MoonLight</a> on an ESP32 microcontroller. You need a compatible browser like Chrome, or Edge, not supported on Safari.

<a href="https://www.youtube.com/watch?v=7DQOEWa-Kwg">
  <img width="380" src="https://img.youtube.com/vi/7DQOEWa-Kwg/maxresdefault.jpg" alt="Watch MoonLight Demo">
</a>

**[▶️ Watch the install tutorial](https://www.youtube.com/watch?v=7DQOEWa-Kwg)** or follow the instructions below

## Prepare an ESP32 device 

See [Hardware](../hardware)

Recommended device to get started: [QuinLED-dig2go](https://quinled.info/quinled-dig2go/)

![Dig2Go](https://shop.allnetchina.cn/cdn/shop/products/Led_4.jpg?v=1680836018&width=1600){: style="width:250px"} 

Connect the device via a USB cable (which supports data transfer) to your PC.

## Flash the firmware

<script type="module" src="https://unpkg.com/esp-web-tools@10/dist/web/install-button.js?module"></script>

Select your ESP32-device from the list below and press the corresponding Connect button and follow these steps:

<img width="250" src="https://github.com/user-attachments/assets/f092743f-a362-40da-b932-d31b203d966d" />

Your device should show up in this list, Connect

<img width="200" alt="Screenshot 2025-06-07 at 20 54 31" src="https://github.com/user-attachments/assets/a0a8d92c-dae2-4cfe-ac78-bc7cacb24724" />
<img width="250" src="https://github.com/user-attachments/assets/35045317-520d-427c-894e-418693877831" />

Press Install MoonLight. Erase when installing MoonLight first time, do not erase if already installed (to preserve settings). Press Next and Install

<img width="200" src="https://github.com/user-attachments/assets/25c05a6c-d2ef-41cb-b83c-fc40a60c6ccc" />
<img width="200" src="https://github.com/user-attachments/assets/b2391752-51c4-400e-b95c-4fa865e93595" />
<img width="200" src="https://github.com/user-attachments/assets/5e2ceefd-4c31-4b72-a228-f29373b677ac" />

Restart the device by going to Logs & Console and press Restart.

| Name | Image* | Flash | Shop & Board presets |
|------|--------|-------|----------------------|
| esp32-d0 | ![esp32-d0](../firmware/installer/images/esp32-d0.jpg){: style="width:100px"} | <esp-web-install-button manifest="https://moonmodules.org/MoonLight/firmware/installer/manifest_esp32-d0.json"></esp-web-install-button> | [Dig Uno](https://quinled.info/pre-assembled-quinled-dig-uno):<br>![Dig Uno](https://quinled.info/wp-content/uploads/2020/02/QuinLED-Dig-Uno-v3_front.png){: style="width:100px"}<br>[Dig Quad](https://quinled.info/pre-assembled-quinled-dig-quad):<br>![Dig Quad](https://quinled.info/wp-content/uploads/2021/11/QuinLED-Dig-Quad-AB_v3r1-2048x1154.png){: style="width:100px"}<br>[Dig2Go](https://quinled.info/quinled-dig2go):<br>![Dig2Go](https://shop.allnetchina.cn/cdn/shop/products/Led_4.jpg?v=1680836018&width=1600){: style="width:100px"} |
| esp32-d0-16mb | ![esp32-d0-16mb](../firmware/installer/images/esp32-d0-16mb.jpg){: style="width:100px"} | <esp-web-install-button manifest="https://moonmodules.org/MoonLight/firmware/installer/manifest_esp32-d0-16mb.json"></esp-web-install-button> | [Dig Octa](https://quinled.info/quinled-dig-octa):<br>![Dig Octa](https://quinled.info/wp-content/uploads/2024/10/20240924_141857-2048x1444.png){: style="width:100px"}<br>[Serg ESP32](https://www.tindie.com/products/serg74/esp32-wroom-usb-c-d1-mini32-form-factor-board){:target="_blank"} and [Shield](https://www.tindie.com/products/serg74/wled-shield-board-for-addressable-leds)<br>![Shield](https://cdn.tindiemedia.com/images/resize/44YE-eNQ9pJQUh_SmtwwfBXFbAE=/p/fit-in/1370x912/filters:fill(fff)/i/93057/products/2021-08-14T14%3A44%3A14.418Z-shield_v3-1.jpg?1628927139){: style="width:100px"} |
| esp32-s3-n8r8v | ![esp32-s3-n8r8v](../firmware/installer/images/esp32-s3-n8r8v.jpg){: style="width:100px"} | <esp-web-install-button manifest="https://moonmodules.org/MoonLight/firmware/installer/manifest_esp32-s3-n8r8v.json"></esp-web-install-button> | SE-16p<br>![SE-16p](../firmware/installer/images/esp32-s3-stephanelec-16p.jpg){: style="width:100px"} |
| esp32-s3-n16r8v | ![esp32-s3-n16r8v](../firmware/installer/images/esp32-s3-n8r8v.jpg){: style="width:100px"} | <esp-web-install-button manifest="https://moonmodules.org/MoonLight/firmware/installer/manifest_esp32-s3-n16r8v.json"></esp-web-install-button> | [Ali*](https://s.click.aliexpress.com/e/_DBAtJ2H){:target="_blank"} |
| esp32-s3-atoms3r | ![esp32-s3-atoms3r](../firmware/installer/images/esp32-s3-atoms3r.jpg){: style="width:100px"} | <esp-web-install-button manifest="https://moonmodules.org/MoonLight/firmware/installer/manifest_esp32-s3-atoms3r.json"></esp-web-install-button> | [M5Stack store](https://shop.m5stack.com/products/atoms3r-dev-kit){:target="_blank"} |
| esp32-s3-zero-n4r2 | ![esp32-s3-zero-n4r2](../firmware/installer/images/esp32-s3-zero-n4r2.jpg){: style="width:100px"} | <esp-web-install-button manifest="https://moonmodules.org/MoonLight/firmware/installer/manifest_esp32-s3-zero-n4r2.json"></esp-web-install-button> | [Ali*](https://s.click.aliexpress.com/e/_EukjHX8){:target="_blank"} |
| esp32-p4-nano | ![esp32-p4-nano](../firmware/installer/images/esp32-p4-nano.jpg){: style="width:100px"} | install [esp32-c6-firmware-update](https://esp32-c6-firmware-update.github.io) first <br><esp-web-install-button manifest="https://moonmodules.org/MoonLight/firmware/installer/manifest_esp32-p4-nano.json"></esp-web-install-button> | [Waveshare](https://www.waveshare.com/esp32-p4-nano.htm){:target="_blank"} <br>[ESP32-P4 shield](https://shop.myhome-control.de/en/ABC-WLED-ESP32-P4-shield/HW10027):<br>![ESP32-P4 shield](https://shop.myhome-control.de/thumbnail/ed/a1/f1/1762031307/WLED_ESP32_P4_Shield_04_1920x1354.jpg?ts=1762031316){: style="width:100px"} |
| esp32-d0-wrover | ![esp32-d0-wrover](../firmware/installer/images/esp32-d0-wrover.jpg){: style="width:100px"} | <esp-web-install-button manifest="https://moonmodules.org/MoonLight/firmware/installer/manifest_esp32-d0-wrover.json"></esp-web-install-button> | [Ali*](https://a.aliexpress.com/_EzhPi6g){:target="_blank"} |
| esp32-p4-olimex | ![esp32-p4-olimex](../firmware/installer/images/esp32-p4-olimex.jpg){: style="width:100px"} | <esp-web-install-button manifest="https://moonmodules.org/MoonLight/firmware/installer/manifest_esp32-p4-olimex.json"></esp-web-install-button> | [Olimex](https://www.olimex.com/Products/IoT/ESP32-P4/ESP32-P4-DevKit/open-source-hardware){:target="_blank"} |
| esp32-c3 | ![esp32-c3](../firmware/installer/images/esp32-c3.jpg){: style="width:100px"} | <esp-web-install-button manifest="https://moonmodules.org/MoonLight/firmware/installer/manifest_esp32-c3.json"></esp-web-install-button> | [Ali*](https://s.click.aliexpress.com/e/_EIl7NKw){:target="_blank"} |
| esp32-c3-supermini | ![esp32-c3-supermini](../firmware/installer/images/esp32-c3-supermini.jpg){: style="width:100px"} | <esp-web-install-button manifest="https://moonmodules.org/MoonLight/firmware/installer/manifest_esp32-c3-supermini.json"></esp-web-install-button> | [Ali*](https://s.click.aliexpress.com/e/_EIl7NKw){:target="_blank"} |
| others | ![others](../firmware/installer/images/others.jpg){: style="width:100px"} | <esp-web-install-button manifest="https://moonmodules.org/MoonLight/firmware/installer/manifest.json"></esp-web-install-button> | |

## Setup MoonLight

A new MoonLight device will show itself as a WiFi Access Point (AP) in the WiFi settings of your PC, tablet or smartphone. The name starts with ML- and 4 characters.

<img width="200" src="https://github.com/user-attachments/assets/9a146e3c-1a53-4906-ad2a-d70215efcf4b" />

Select ML-xxxx, MoonLight will start in a browser:

<img width="350" src="../../media/moonlight/ML-start.png"/>

Follow the steps:

### WiFi connection

Add a [WiFi](../../network/sta) connection

<img width="350" src="../../media/network/MB-WiFi2.png"/>

Enter a hostname, add your WiFi by adding the (+) button, enter SSID and password and press Apply Settings.

Restart the device by pressing the boot button or simple unplug and plug the usb cable.

Go back to your home WiFi open a browser and enter hostname.local to open MoonLight via the new WiFi connection.

### IO Board Presets

Select the specific board you are running in [Module IO](../../moonbase/inputoutput), e.g. premade or custom boards as shown in [hardware](../hardware).

<img width="320" height="441" alt="io11" src="../../media/moonbase/inputoutput.png" />

Make sure the defined LED pin(s) are the same as the physical connections on the ESP32 board.

### Drivers

The drivers module contains [Layouts](../../moonlight/layouts) and [Drivers](../../moonlight/drivers).

Add a layout first, this defines how your LEDs are arranged. Press (+)

<img width="350" src="https://github.com/user-attachments/assets/8d8b9c2d-bed3-439e-a145-2bb204639c6c" />

For LED strips, choose Single Column, for Panels choose Panel(s) or other layouts. Layouts are recognized by 🚥. Enter the size of your LEDs

Add the LED driver by pressing the blue + button and select the Physical LED driver or FastLED driver. Drivers are recognized by ☸️

<img width="350" src="https://github.com/user-attachments/assets/e7fab24f-3803-4aa5-8638-459cca8a9caf" />

### Effect

The Effects module contains [Effects](../../moonlight/effects) and [Modifiers](../../moonlight/modifiers). Start by adding one effect, press (+)

<img width="350" src="https://github.com/user-attachments/assets/2c8a8b75-c429-4038-a5af-adc82b11c9de" />

The effect should be visible on the Monitor in MoonLight and on your LEDs

### Save and next steps

Press save (💾). Saves your setup to the file system, so if a device is restarted, your settings are still there.

If anything not working as expected, log an issue on GitHUB or contact us on Discord, Reddit or YouTube. See also [Installer troubleshooting](../../develop/installation/#moonlight-installer) and [USB-to-serial chip drivers](../../develop/installation/#usb-to-serial-chip-drivers)

!!! info "Next steps"
    * You can now change effect or add effects, modifiers, layouts and drivers
    * Go to [MoonLight Overview](../../moonlight/overview) to learn more
    * press the (?) in MoonLight to go directly to the relevant page

## Update MoonLight

MoonLight releases new versions regularly. As it is a new and growing platform it is recommended to update as soon as new releases are published. They can be installed via above procedure (without erase) or via one of the following procedures:

### System update

The preferred way to update MoonLight is directly in MoonLight via [System update](../../system/update). MoonLight notifies when new versions are available.

<img width="350" src="https://github.com/user-attachments/assets/523ea32f-88f8-4994-8d23-9541dce67ba1" />

Currently not supported on smaller devices like the ESP32-D0 and ESP32-C3. Use the MoonLight Installer in this case.

### ESPConnect

Use ESPConnect to install unreleased firmware e.g. provided via [discord](https://discord.gg/MTn9mVUG5n) or GitHub issues. Published [releases](https://github.com/MoonModules/MoonLight/releases) can also be installed via this way.

Launch [ESPConnect](https://thelastoutpostworkshop.github.io/microcontroller_devkit/espconnect)

<img width="400" src="https://github.com/user-attachments/assets/3b702b3e-62cc-4330-83f6-d473067df449" />

Click Connect and choose your device. Select Flash Tools, go to Flash Firmware, open a firmware.bin file from your file system, select Flash Offset App0 and Flash.
 
### Visual Studio code

For developers. Upload directly from VSCode, see [Develop / Installation](../../develop/installation)

*: This page contains affiliate links
