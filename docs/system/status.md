# System Status

<img width="320" src="https://github.com/user-attachments/assets/ebbe9723-de0b-4fcf-a527-b660b508bb6e" />

* 🌙 **Performance**: loops per second for effects, drivers and overall. The Effects and Drivers rows show the theoretical maximum loop speed for each task — the speed those tasks could reach if they were the only thing running. Actual speed (shown as lps) may be lower due to shared CPU time.
* 🌙 **Safe Mode**: After a crash, the device will start in Safe Mode disabling possible causes of crashes. See also [MoonLight](https://moonmodules.org/MoonLight/moonlight/overview/). In case of safe mode, the statusbar will show a shield: 🛡️. Try to find the reason of the crash and correct and restart the device. If no crash, it will go out of safe mode.
* 🌙 **Firmware Target**: Which firmware has been installed, see [MoonLight Installer](https://moonmodules.org/MoonLight/gettingstarted/installer/)
* 🌙 **Firmware Date**: What is the date the firmware is created (format YYYYMMDDHH)

* **Sleep**: Device is set in low power mode.
* **Restart**: The device will restart
* **Factory reset**: all settings will be removed.

On ESP32-P4 boards with an onboard Wi-Fi coprocessor, the coprocessor firmware version is shown in System Status.

