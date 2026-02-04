# MoonLight Installer

Install <a href="https://github.com/MoonModules/MoonLight" target="_blank">MoonModules/MoonLight</a> on an ESP32 microcontroller.  
You need a Web Serial–compatible browser such as **Chrome** or **Edge**. **Safari is not supported.**

For devices that already have MoonLight installed, go to [Update](#update-moonlight).

<a href="https://www.youtube.com/watch?v=7DQOEWa-Kwg">
  <img width="380" src="https://img.youtube.com/vi/7DQOEWa-Kwg/maxresdefault.jpg" alt="Watch MoonLight Demo">
</a>

**[▶️ Watch the install tutorial](https://www.youtube.com/watch?v=7DQOEWa-Kwg)** (v0.6.0), or follow the instructions below.

## Prepare an ESP32 device

Recommended plug-and-play device to get started with, up to 1024 LEDs @ 30FPS (one output) or 4096 LEDs over the network (Art-Net): [QuinLED-dig2go](https://quinled.info/quinled-dig2go/)

![Dig2Go](https://shop.allnetchina.cn/cdn/shop/products/Led_4.jpg?v=1680836018&width=1600){: style="width:250px"}

Recommended low-cost DIY board up to 10K LEDs: [ESP32-S3 n16r8](https://s.click.aliexpress.com/e/_DBAtJ2H){:target="_blank"} 

![esp32-s3-n16r8v](../firmware/installer/images/esp32-s3-n8r8v.jpg){: style="width:100px"}

Recommended state-of-the-art DIY board, up to 16-98K LEDs: [ESP32-P4-Nano](https://www.waveshare.com/esp32-p4-nano.htm){:target="_blank"}

![esp32-p4-nano](../firmware/installer/images/esp32-p4-nano.jpg){: style="width:100px"}

See [Hardware](../hardware) for other options.

Connect the device to your PC using a USB cable that **supports data transfer**.

## Flash the firmware

<script type="module" src="https://unpkg.com/esp-web-tools@10/dist/web/install-button.js?module"></script>

Select your ESP32 device from the table below, press the corresponding **Connect** button, and follow the steps.  
Your device should appear in the list.

When installing MoonLight for the **first time**, enable the **Erase** checkbox.  
After a successful install, go to **Logs & Console**, press **Reset Device**, and close the installer.

!!! info "Bootloader mode"
    If installation does not work, some boards must be placed into bootloader mode the first time:
    press and hold the `Boot` button on the ESP32, press the `Reset` button, then release the `Boot` button.

| Name | Image* | Flash | Shop & Board presets |
|------|--------|-------|----------------------|
| esp32-d0 | ![esp32-d0](../firmware/installer/images/esp32-d0.jpg){: style="width:100px"} | <esp-web-install-button manifest="https://moonmodules.org/MoonLight/firmware/installer/manifest_esp32-d0.json"></esp-web-install-button> | [Dig2Go](https://quinled.info/quinled-dig2go):<br>![Dig2Go](https://shop.allnetchina.cn/cdn/shop/products/Led_4.jpg?v=1680836018&width=1600){: style="width:100px"}<br>[Dig Uno](https://quinled.info/pre-assembled-quinled-dig-uno):<br>![Dig Uno](https://quinled.info/wp-content/uploads/2020/02/QuinLED-Dig-Uno-v3_front.png){: style="width:100px"}<br>[Dig Quad](https://quinled.info/pre-assembled-quinled-dig-quad):<br>![Dig Quad](https://quinled.info/wp-content/uploads/2021/11/QuinLED-Dig-Quad-AB_v3r1-2048x1154.png){: style="width:100px"} |
| esp32-d0-16mb | ![esp32-d0-16mb](../firmware/installer/images/esp32-d0-16mb.jpg){: style="width:100px"} | <esp-web-install-button manifest="https://moonmodules.org/MoonLight/firmware/installer/manifest_esp32-d0-16mb.json"></esp-web-install-button> | [Dig Octa](https://quinled.info/quinled-dig-octa):<br>![Dig Octa](https://quinled.info/wp-content/uploads/2024/10/20240924_141857-2048x1444.png){: style="width:100px"}<br>[Serg ESP32](https://www.tindie.com/products/serg74/esp32-wroom-usb-c-d1-mini32-form-factor-board){:target="_blank"} and [Shield](https://www.tindie.com/products/serg74/wled-shield-board-for-addressable-leds)<br>![Shield](https://cdn.tindiemedia.com/images/resize/44YE-eNQ9pJQUh_SmtwwfBXFbAE=/p/fit-in/1370x912/filters:fill(fff)/i/93057/products/2021-08-14T14%3A44%3A14.418Z-shield_v3-1.jpg?1628927139){: style="width:100px"} |
| [esp32-d0-pico2](https://documentation.espressif.com/esp32-pico-mini-02_datasheet_en.pdf) | ![esp32-d0-pico2](../firmware/installer/images/esp32-d0-pico2.jpg){: style="width:100px"} | <esp-web-install-button manifest="https://moonmodules.org/MoonLight/firmware/installer/manifest_esp32-d0-pico2.json"></esp-web-install-button> | [DigNext2](https://quinled.info/dig-next-2):<br>![DigNext2](https://quinled.info/wp-content/uploads/2026/01/P1087754-Enhanced-NR-2560x1358.jpg){: style="width:100px"} |
| esp32-s3-n8r8v | ![esp32-s3-n8r8v](../firmware/installer/images/esp32-s3-n8r8v.jpg){: style="width:100px"} | <esp-web-install-button manifest="https://moonmodules.org/MoonLight/firmware/installer/manifest_esp32-s3-n8r8v.json"></esp-web-install-button><br><esp-web-install-button manifest="https://moonmodules.org/MoonLight/firmware/installer/manifest_esp32-s3-n8r8v-nightly.json">Nightly</esp-web-install-button> | SE-16p<br>![SE-16p](../firmware/installer/images/esp32-s3-stephanelec-16p.jpg){: style="width:100px"} |
| esp32-s3-n16r8v | ![esp32-s3-n16r8v](../firmware/installer/images/esp32-s3-n8r8v.jpg){: style="width:100px"} | <esp-web-install-button manifest="https://moonmodules.org/MoonLight/firmware/installer/manifest_esp32-s3-n16r8v.json"></esp-web-install-button><br><esp-web-install-button manifest="https://moonmodules.org/MoonLight/firmware/installer/manifest_esp32-s3-n16r8v-nightly.json">Nightly</esp-web-install-button> | [Ali*](https://s.click.aliexpress.com/e/_DBAtJ2H){:target="_blank"} |
| esp32-s3-atoms3r | ![esp32-s3-atoms3r](../firmware/installer/images/esp32-s3-atoms3r.jpg){: style="width:100px"} | <esp-web-install-button manifest="https://moonmodules.org/MoonLight/firmware/installer/manifest_esp32-s3-atoms3r.json"></esp-web-install-button> | [M5Stack store](https://shop.m5stack.com/products/atoms3r-dev-kit){:target="_blank"} |
| esp32-s3-zero-n4r2 | ![esp32-s3-zero-n4r2](../firmware/installer/images/esp32-s3-zero-n4r2.jpg){: style="width:100px"} | <esp-web-install-button manifest="https://moonmodules.org/MoonLight/firmware/installer/manifest_esp32-s3-zero-n4r2.json"></esp-web-install-button> | [Ali*](https://s.click.aliexpress.com/e/_EukjHX8){:target="_blank"} |
| esp32-p4-nano | ![esp32-p4-nano](../firmware/installer/images/esp32-p4-nano.jpg){: style="width:100px"} | Install [esp32-c6-firmware-update](https://esp32-c6-firmware-update.github.io) first<br><esp-web-install-button manifest="https://moonmodules.org/MoonLight/firmware/installer/manifest_esp32-p4-nano.json"></esp-web-install-button> | [Waveshare](https://www.waveshare.com/esp32-p4-nano.htm){:target="_blank"}<br>[ESP32-P4 shield](https://shop.myhome-control.de/en/ABC-WLED-ESP32-P4-shield/HW10027):<br>![ESP32-P4 shield](https://shop.myhome-control.de/thumbnail/ed/a1/f1/1762031307/WLED_ESP32_P4_Shield_04_1920x1354.jpg?ts=1762031316){: style="width:100px"} |
| esp32-d0-wrover | ![esp32-d0-wrover](../firmware/installer/images/esp32-d0-wrover.jpg){: style="width:100px"} | <esp-web-install-button manifest="https://moonmodules.org/MoonLight/firmware/installer/manifest_esp32-d0-wrover.json"></esp-web-install-button> | [Ali*](https://a.aliexpress.com/_EzhPi6g){:target="_blank"} |
| esp32-p4-olimex | ![esp32-p4-olimex](../firmware/installer/images/esp32-p4-olimex.jpg){: style="width:100px"} | <esp-web-install-button manifest="https://moonmodules.org/MoonLight/firmware/installer/manifest_esp32-p4-olimex.json"></esp-web-install-button> | [Olimex](https://www.olimex.com/Products/IoT/ESP32-P4/ESP32-P4-DevKit/open-source-hardware){:target="_blank"} |
| esp32-c3 | ![esp32-c3](../firmware/installer/images/esp32-c3.jpg){: style="width:100px"} | <esp-web-install-button manifest="https://moonmodules.org/MoonLight/firmware/installer/manifest_esp32-c3.json"></esp-web-install-button> | [Ali*](https://s.click.aliexpress.com/e/_EIl7NKw){:target="_blank"} |
| esp32-c3-supermini | ![esp32-c3-supermini](../firmware/installer/images/esp32-c3-supermini.jpg){: style="width:100px"} | <esp-web-install-button manifest="https://moonmodules.org/MoonLight/firmware/installer/manifest_esp32-c3-supermini.json"></esp-web-install-button> | [Ali*](https://s.click.aliexpress.com/e/_EIl7NKw){:target="_blank"} |
| esp32-d0-moonbase | ![esp32-d0-moonbase](../firmware/installer/images/esp32-d0.jpg){: style="width:100px"} | <esp-web-install-button manifest="https://moonmodules.org/MoonLight/firmware/installer/manifest_esp32-d0-moonbase.json"></esp-web-install-button> | |

Install steps in pictures:

<img width="250" src="https://github.com/user-attachments/assets/f092743f-a362-40da-b932-d31b203d966d" />
<img width="200" src="https://github.com/user-attachments/assets/a0a8d92c-dae2-4cfe-ac78-bc7cacb24724" />
<img width="250" src="https://github.com/user-attachments/assets/35045317-520d-427c-894e-418693877831" />
<img width="200" src="https://github.com/user-attachments/assets/25c05a6c-d2ef-41cb-b83c-fc40a60c6ccc" />
<img width="200" src="https://github.com/user-attachments/assets/b2391752-51c4-400e-b95c-4fa865e93595" />
<img width="200" src="https://github.com/user-attachments/assets/5e2ceefd-4c31-4b72-a228-f29373b677ac" />

## Setup MoonLight

A new MoonLight device will appear as a WiFi Access Point (AP) in the WiFi settings of your PC, tablet, or smartphone.  
The name starts with `ML-` followed by four characters.

<img width="200" src="https://github.com/user-attachments/assets/9a146e3c-1a53-4906-ad2a-d70215efcf4b" />

Select `ML-xxxx`. MoonLight will open in your browser:

<img width="350" src="../../media/moonlight/ML-Start.png"/>

Follow the steps below.

### WiFi connection

Add a [WiFi](../../network/sta) connection.

<img width="350" src="../../media/network/MB-WiFi2.png"/>

Enter a hostname, add your WiFi network using the (+) button, enter the SSID and password, and press **Apply Settings**.

Restart the device by pressing the boot button, or simply unplug and reconnect the USB cable.

Reconnect to your home WiFi, open a browser, and go to `hostname.local` to access MoonLight via the new WiFi connection.

### IO Board Presets

Select your board in [Module IO](../../moonbase/inputoutput).  
If you have one of the supported premade boards (see table above), select it.  
If you are unsure, leave the default settings.

<img width="320" height="441" src="../../media/moonbase/inputoutput.png" />

Ensure the configured LED pin(s) match the physical connections on your ESP32 board.

### Drivers

The Drivers module contains [Layouts](../../moonlight/layouts) and [Drivers](../../moonlight/drivers).

Layouts define how your LEDs are arranged.  
First, add a layout by pressing (+).

<img width="320" src="../../media/moonlight/layouts/PanelLayout.png"/>

For LED strips, choose **Single Column**.  
For panels, choose **Panel(s)** or another suitable layout.  
Layouts are indicated by 🚥. Enter the size of your LED setup.

Next, add an LED driver by pressing (+) and selecting either the **Physical LED driver** or **FastLED driver**.  
Drivers are indicated by ☸️.

<img width="350" src="https://github.com/user-attachments/assets/e7fab24f-3803-4aa5-8638-459cca8a9caf" />

### Effect

The Effects module contains [Effects](../../moonlight/effects) and [Modifiers](../../moonlight/modifiers).

Start by adding an effect using the (+) button.

<img width="350" src="https://github.com/user-attachments/assets/2c8a8b75-c429-4038-a5af-adc82b11c9de" />

The effect should now be visible in the MoonLight monitor and on your LEDs.

<img width="320" src="../../media/moonlight/effects/LissajousShot.png"/>

### Save and next steps

Press **Save** (💾).  
This stores your configuration on the device, so your settings persist after a reboot.

If something does not work as expected, log an issue on GitHub or contact us via [Discord](https://discord.gg/TC8NSUSCdV), [Reddit](https://reddit.com/r/moonmodules) or [YouTube](https://www.youtube.com/@MoonModulesLighting).  
See also [Installer troubleshooting](../../develop/installation/#moonlight-installer) and [USB-to-serial chip drivers](../../develop/installation/#usb-to-serial-chip-drivers).

!!! info "Next steps"
    * You can now change or add effects, modifiers, layouts, and drivers
    * Visit the [MoonLight Overview](../../moonlight/overview) to learn more
    * Press (?) in MoonLight to jump directly to the relevant documentation page

## Update MoonLight

MoonLight releases new versions regularly.  
As this is a new and growing platform, it is recommended to update as soon as new releases are published.

Updates can be installed using the installer above (without erasing), or via one of the methods below.

### System update

The preferred way to update MoonLight is directly within MoonLight via [System update](../../system/update).  
MoonLight will notify you when new versions are available.

<img width="350" src="https://github.com/user-attachments/assets/523ea32f-88f8-4994-8d23-9541dce67ba1" />

This method is currently not supported on smaller devices such as the ESP32-D0 and ESP32-C3.  
Use the MoonLight Installer instead for these devices.

### ESPConnect

Use ESPConnect to install unreleased firmware (for example, versions shared via [Discord](https://discord.gg/TC8NSUSCdV) or [GitHub issues](https://github.com/MoonModules/MoonLight/issues)).  
Published [releases](https://github.com/MoonModules/MoonLight/releases) can also be installed this way.

Launch [ESPConnect](https://thelastoutpostworkshop.github.io/microcontroller_devkit/espconnect).

<img width="400" src="https://github.com/user-attachments/assets/3b702b3e-62cc-4330-83f6-d473067df449" />

Click **Connect** and select your device.  
Choose **Flash Tools**, go to **Flash Firmware**, open a `firmware.bin` file from your file system, select **Flash Offset App0**, and click **Flash**.

### Visual Studio Code

For developers: upload firmware directly from VS Code.  
See [Develop / Installation](../../develop/installation).

\*: This page contains affiliate links
