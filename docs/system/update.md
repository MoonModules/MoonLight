# System Update

<img width="320" height="441" src="../update.png" />

This module supports Over the Air Updates (OTA).
There are 2 ways to update the firmware in this module:

* Using GitHub releases

     * List of firmware releases is showed here
     * Select one of the releases to install. In general it is advised to install the most recent version.
     * If there is a new release, the app will notify it's availability.
     * See here for details on releases: [releases](https://github.com/MoonModules/MoonLight/releases)
     * The Nightly release is experimental, use it to test new features, see the Test Channel on the MoonLight [Discord server](https://discord.gg/TC8NSUSCdV) for new functionality to test.

     <img width="362" src="https://github.com/user-attachments/assets/cf72875f-02cf-4c78-97a0-56b2a1723d71" />

* Manually via Upload

    * Select choose file, Press Upload if asked for Are you sure?. 

When starting the update, a progress indicator is shown, upon completion the device will restart with the new firmware.

!!! warning "Uptate on small devices not possible"
    On some devices firmware update using above methods is not possible (yet) due to a limited amount of file size or memory (ESP32-D0 devices). Use the [Installer](https://moonmodules.org/MoonLight/gettingstarted/installation/) procedure instead (without erasing the device)
