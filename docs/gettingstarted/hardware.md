# Hardware

To get started the minimal you need is an ESP32-device connected via USB to your computer. The ESP32 can drive LEDs directly or can use Art-Net over the network to Art-Net controllers.

If driving LEDs directly, one or more LED-strips or panels should be connected via 3 wires to the vcc, ground and a supported GPIO pin of the ESP32. 

It's most easy to start with pre-made boards, however, you can also DIY.

## Premade ESP32 devices

See [IO module](../../moonbase/inputoutput) for premade devices which can be preconfigured in MoonLight.

![Dig2Go](https://shop.allnetchina.cn/cdn/shop/products/Led_4.jpg?v=1680836018&width=1600){: style="width:250px"} 

* [QuinLed LED pre assembled boards](https://quinled.info/pre-assembled-boards/). The Dig2Go is the easiest complete package to use.
* [Serg74 shields](https://www.tindie.com/products/serg74/esp32-wroom-usb-c-d1-mini32-form-factor-board/)
* [Home control](https://www.myhome-control.de/), e.g. the ESP32-P4 Nano shield
* [Athom](https://www.athom.tech/wled) / [Gledopto](https://www.gledopto.eu/Gledopto-WLED-LED-Controller-for-Digital-Light-Strips) / [M5Stack](https://shop.m5stack.com/collections/m5-controllers) / ...

## DIY

### Minimal hardware

* One of the ESP32-devices shown in [MoonLight Installer](../installer). Any ESP32-S3 board or ESP32-P4 nano recommended as they have more memory (PSRAM) then the traditional ESP32-D0 boards.
* Any WS2812 LED strip or panel. [256 LED panel](https://s.click.aliexpress.com/e/_EIKoYrg) recommended.
* Connect the ESP32-device with USB to your computer, connect the LED strip or panel

<img width="350" src="https://github.com/user-attachments/assets/1623a751-5f4b-463b-a6f3-a642c2bc52bf"/>

Above is the minimum requirement to have lights burning.

### Recommended hardware

The [M5Stack AtomS3R](https://docs.m5stack.com/en/core/AtomS3R) can be used to offer 'MoonLight in a box'. 
It is perfactly usable to drive Art-Net. To drive LEDs a few more components are needed:

| Product | Image | Shop |
| ---- | ----- | ---- |
| [M5Stack AtomS3R](https://docs.m5stack.com/en/core/AtomS3R) | <img width="200" src="/firmware/installer/images/esp32-s3-atoms3r.jpg"/>  | <a href="https://shop.m5stack.com/products/atoms3r-dev-kit" target="_blank">Shop</a> |
|Level shifter SN74HCT125(N)|| [Shop](https://s.click.aliexpress.com/e/_EIVbQ2w)|
| Resistors: 33 or 249 ohm || see [QuinLed - The Myth of the Data Signal Resistor](https://quinled.info/data-signal-cable-conditioning/)|
| External power supply |||
| Capacitors |||

Currently it is DIY to make it:

<img width="140" src="https://github.com/user-attachments/assets/9cbe487e-f330-40a5-8b40-6663c83e5d90" />
<img width="120" src="https://github.com/user-attachments/assets/adca881f-23d9-4aea-ab41-c02ae9005da1" />
<img width="100" src="https://github.com/user-attachments/assets/9308d655-1ab9-477d-a7f3-b510df44e9d6" />

Work in progress to make this available as pre-assembled board ([Contact](https://discord.gg/TC8NSUSCdV) us if you want to help manufactering this).

## DMX / Art-Net

A few examples are shown below. Note that MoonLight also supports Art-Net in which can be uased as an alternative to off-the-shelve controllers. In this scenario there is one MoonLight device sending Art-Net and one (or more) MoonLight devices receiving Art-Net

| Product | Image | Shop |
| ---- | ----- | ---- |
|Artnet-LED-controller|<img width="200" src="https://github.com/user-attachments/assets/9c65921c-64e9-4558-b6ef-aed2a163fd88">|[Shop](https://s.click.aliexpress.com/e/_Ex9uaOk)|
| Pknight Art-Net DMX 512 | <img width="200" src="https://github.com/user-attachments/assets/e3d605b6-a023-4abb-b604-77b44267b1a3"> | [Shop](https://s.click.aliexpress.com/e/_ExQK8Dc) |
|18 LED Bars|<img width="300" src="https://github.com/user-attachments/assets/75b18cf6-bc32-4bf0-a03b-7eea8dbfd677" />| [Shop](https://s.click.aliexpress.com/e/_EQMKbmK) |
|19x15W Zoom Wash Lights RGBW Beam Moving Head|<img width="300" src="https://github.com/user-attachments/assets/6e61c41f-e128-4adc-b9c1-6239fe4736dc" />|[Shop](https://s.click.aliexpress.com/e/_EwBfFYw)|

For an impression of hardware in use:

<a href="https://www.youtube.com/watch?v=NnEV9RplZN8">
  <img width="380" src="https://img.youtube.com/vi/NnEV9RplZN8/maxresdefault.jpg" alt="Watch MoonLight Demo">
</a>

**[▶️ Watch MoonLight in Concert ](https://www.youtube.com/watch?v=NnEV9RplZN8)**
