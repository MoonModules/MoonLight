# IO Module

<img width="320" height="441" alt="io11" src="https://github.com/user-attachments/assets/b5452d8e-400c-4a72-a195-d5b7a7764407" />
<img width="320" height="77" alt="io22" src="https://github.com/user-attachments/assets/99fd135c-8d36-463b-ae3a-f06119cb8ccb" />

Defines hardware specifics per ESP32 device 🆕
Currently the following boards are defined. Not all are supported yet 🚧

<Image>

For each board the following presets are defined:

* Modded: if any change to the default preset is made.
* Max Power: adjust the brightness to approach this max power, depending on the number of LEDs used.
* Jumper1: If the board contains a jumper, it can define pin behaviour. Eg. select between Infrared and Ethernet.
* Pins: This module is the central place to assign functionality to gpio pins. Other modules and nodes use the pin assignments made here.

* Per pin:
    * GPIO = gpio_num;
    * Usage: Can be updated, see below
    * Summary
       * Valid (✅)
       * Output (💡)
       * RTC (⏰);
    * Level (HIGH, LOW, N/A)
    * DriveCap (WEAK, STRONGER, MEDIUM, STRONGEST, N/A)

<image>

## Pin usage

  * Supported
    * LEDs DIO 01 to 20
    * Voltage
    * Current
    * Infrared
    * Button On/Off
  * Planned soon
    * Battery
    * Relay brightness
    * DMX (in)
  * Planned later
    * I2S for microphone and line in
    * I2C

## Naming convention

- MicroController (MCU): The ESP32 chip
- MCU-Board (MCB): MCU on a PCB
- Carrier Board (CRB): board that the MCU-board plugs into (Or shield or controller board or interface board)
- Device (DVC): All of the above in a box with connectors

## Board details

### QuinLed boards

* esp32-d0 (4MB)
* erase flash first as MoonLight uses a partition scheme with 3MB of flash and no ota.
* Lolin Wifi fix no -> add as board preset
* you might need to reset your router if you first run WLED on the same MCU
* remove fuse then flash, Nice !!!

### SE16 v1

* Set jumper1 the same as you set it on the board: on: Infrared, off: Ethernet