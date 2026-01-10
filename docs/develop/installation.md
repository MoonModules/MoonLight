
# Installation

The development environment consists of

* VSCode
    * Platformio IDE
    * nodejs
    * git
    * python

* Github manager
    * GitKraken (recommended)
    * GitHub Desktop
    * No Github manager (not recommended)

## Summary

* Fork the MoonLight repo and download
* Install VSCode with the platformIO IDE extension
* Open MoonLight in VSCode. 
* Build or upload to an ESP32 device. 

## Fork and Download

**Step 1**: Create a fork

    * Login to your own github account
    * Fork: go to [MoonModules/MoonLight](https://github.com/MoonModules/MoonLight/) and press Fork, uncheck 'Copy the main branch only' and press Create Fork. You will be moved to your fork of MoonLight

**Step 2**: Download the MoonLight repository

* Use [GitKraken](https://www.gitkraken.com/download) or GitHub Desktop (or manually)
    * Create a folder to download to: e.g. Developer/GitGub/MoonModules/MoonLight
    * Press + / New tab and select Clone a Repo 
    * Copy https://github.com/<your forked repo>/MoonLight.git and paste in the URL field of GitKraken
    * Press clone the repo

    <img width="320" src="https://github.com/user-attachments/assets/74928dac-d59b-4489-b97b-759c6d792b77" />

    * The main branch will be checked out per default. Latest development will be done in branches from main, merged back into main when done.

## Install and setup VSCode

**Step 1**: Download Visual Studio Code

* Windows: download from the Microsoft Store

    <img width="320" src="https://github.com/user-attachments/assets/ff6be5d6-40b2-48ae-9f17-89fc5bcfd848" />

* MacOS: [Visual Studio download](https://code.visualstudio.com/download)

**Step 2**: Install PlatformIO IDE

* In VSCode, search for the extension

    <img width="320" src="https://github.com/user-attachments/assets/d91ab6b0-aeeb-42ed-8a85-d608b88c6103" />

* Install the extension. Please note it can take a while before Configuring project is finished. Do not cancel. Don't change anything in the code yet. Drink a ☕️.

    <img width="320" src="https://github.com/user-attachments/assets/ea953f62-0d94-499c-92f0-491d31c2edff" />

!!! info "pioarduino IDE"

     PlatformIO IDE is default but as we are using latest esp-idf, pioarduino IDE is needed when PlatformIO IDE fails.

**Step 3**: * Open the downloaded MoonLight repository 


## Build and upload

**Step 1**: Build

* Press ☑️ in the bottom statusbar

    <img width="320" src="https://github.com/user-attachments/assets/4e6faf4a-f169-46e4-a2f8-5d4021516a04" />


* If you compile for the first time it take some time to finish (☕️)

    <img width="320" src="https://github.com/user-attachments/assets/c5655c00-6b91-4745-ae22-39f8573ae05a" />
    <img width="320" src="https://github.com/user-attachments/assets/2d21319d-2e86-473e-9e5a-d500ecb462c8" />

**Step 2**: Upload MoonLight to an ESP32-device

* Connect an ESP32-device via USB and select the device using the (second) 🔌 icon in the staturbar. See [Hardware](https://moonmodules.org/MoonLight/gettingstarted/hardware/).   

!!! info "USB-to-serial chip drivers"

    In some cases, ESP32-devices won't show connected. See USB-to-serial chip drivers at [MoonLight Installer](https://moonmodules.org/MoonLight/gettingstarted/installer/) 

* Erase the device if it is a new device or the device has not been used for MoonModules before: go to 👽 in the left menu and select Erase Flash

    <img width="350" src="https://github.com/user-attachments/assets/322bb30e-2709-43de-bd68-80044c9a0673" />

* Press upload (➡️) in the status bar

    <img width="350" src="https://github.com/user-attachments/assets/e4cbda64-739c-410d-9cdc-d2645e24c7ba" />

* The firmware is now flashed to your ESP32 device, after flashing the ESP32 device will restart

!!! tip "Serial Monitor"
    Recommended: Press PlatformIO:Serial Monitor to see the debug information produced (the first 🔌 on the VSCode statusbar)

    <img width="350" src="https://github.com/user-attachments/assets/c925d883-173b-4288-89d4-4770c2d86a02" />

**Step 3**: Connect to MoonLight

Before changing code, test if the current download of MoonLight is running fine. Follow the instructions in [Connect MoonLight](https://moonmodules.org/MoonLight/gettingstarted/installation/#connect-moonlight) and [Setup MoonLight](https://moonmodules.org/MoonLight/gettingstarted/installation/#setup-moonlight).

* If you are developing or updating existing MoonLight installations MoonLight might be outdated or not show up correctly in the browser or not even appear. Two reasons

    * Sometimes the latest front-end code is not generated yet (WWWData.h). This is how to set it right: 
        * Open platformIO new terminal (>_)
        * touch ./interface/src/app.html so the build process will be triggered to create a new WWWData.h
        * build the project (✔) - if nodejs is not installed (yet) you will get errors. See troubleshooting
        * check in your github manager (gitkraken of github desktop) that a new WWWData.h is created

    * An old version is cached in the browser. See [UI not showing when installing new version of MoonLight](https://moonmodules.org/MoonLight/gettingstarted/installation/#connect-moonlight) how to solve that.

* A MoonLight device can be accessed via it's IP address or via [http://ml-home.local](http://ml-home.local). The latter uses MDNS.

!!! tip "ESP32Devices"
    IF you want to know which MoonLight (or WLED) devices run on your network, use [ESP32Devices](https://github.com/ewowi/ESP32Devices) to discover the ESP32 nodes on your network

## Troubleshooting

!!! info "supporting tools"

     * Optionally you need to install git and nodejs, run npm install and make sure python > v3.10 in VSCode.
     * Instead of Platformio IDE, the pioarduino IDE extension might be needed in VSCode

### Git install

* When git is not installed on your system, [download git](https://git-scm.com/downloads) and install, restart VSCode and press ☑️ again

    * Windows:

        <img width="320" src="https://github.com/user-attachments/assets/8d39c637-c7dc-4c34-b177-f8eb63d54863" />
    
    * MacOS

        <img width="320" src="https://github.com/user-attachments/assets/b6c27943-6046-4fd7-aab4-3c91544f761d" />

!!! info "Install git on MacOS"

    * Install [homebrew](https://brew.sh): will take a while: ☕️
    * Install git using homebrew (replace <user> with your user name):
     
    ```
    /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

    echo >> /Users/<user>/.zprofile
    echo 'eval "$(/opt/homebrew/bin/brew shellenv)"' >> /Users/<user>/.zprofile
    eval "$(/opt/homebrew/bin/brew shellenv)"

    brew install git
    ```

### Python and LZMA

!!! warning "MacOS: Python and LZMA"

    In some cases LZMA support must be installed

    <img width="320" height="50" alt="Screenshot 2025-09-29 at 13 58 07" src="https://github.com/user-attachments/assets/2eef658f-f297-46cd-bbd5-4d769dbf9e56" />

    * Run in VSCode / pio shell ([>_])

    ```
    brew install xz

    python -V
    ```

    * python -V should show a version > 3.10 (e.g. 3.11.7)



### Install nodejs

nodejs is needed if changes on the front-end (UI) are made (interface folder). On each file change, ☑️ or ➡️ will rebuild the UI using nodejs.

* Windows: if nodejs is not on your system you will get this error:

    <img width="640" height="221" alt="Screenshot 2025-09-25 212255" src="https://github.com/user-attachments/assets/f05ca8d2-6485-4965-8b75-edd4b468ddf4" />

    * download [nodejs](https://nodejs.org/en/download) if it is not run in admin mode you will get this:

    <img width="640" height="501" alt="Screenshot 2025-09-25 212842" src="https://github.com/user-attachments/assets/9945217f-7b4a-4b97-87df-3b35b61ccc0a" />

    * if this is the case, run an administrator command prompt and run the downloaded .msi file as follows:

    <img width="649" height="510" alt="Screenshot 2025-09-25 220442" src="https://github.com/user-attachments/assets/144cce1d-bc59-471c-ace3-aa38c52629e4" />
    <img width="640" height="498" alt="Screenshot 2025-09-25 214052" src="https://github.com/user-attachments/assets/affb20e8-785e-471d-98d9-855e5783ce5e" />

    * after succesful install of nodejs run ☑️ or ➡️ again. It might be needed to rebuild node_modules and package-lock.json if you see this

    <img width="640" height="688" alt="Screenshot 2025-09-25 214700" src="https://github.com/user-attachments/assets/b92ccba0-17e1-4434-929e-302ec8afd96e" />

    * Remove node_modules and package-lock.json - run as administrator if the OS complains!
    * Rebuild node_modules and package-lock.json by opening a VSCode shell using [>_] in the statusbar
``` 
cd interface 
npm install
```
    * If that give errors set execution policies as follows:

    <img width="640" height="398" alt="Screenshot 2025-09-25 215039" src="https://github.com/user-attachments/assets/629bc8de-d1ce-4d2d-9a92-e08bc9d31d4b" />

    * Press run ☑️ or ➡️ again it should now complete succesfully

    <img width="640" height="571" alt="Screenshot 2025-09-25 220023" src="https://github.com/user-attachments/assets/cd6754f4-d6df-446a-94fe-c3d7f491be59" />

* MacOS

    * Go to [nodejs download](https://nodejs.org/en/download)
    * Get a prebuilt Node.js® for macOS running a ARM64 architecture (for modern Mx MacBooks)
    * Choose maxOS Installer (.pkg)
    * Next Next Finish Done
    * Press run ☑️ or ➡️ again it should now complete succesfully (if needed restart VSCode)
    * If still errors, Remove node_modules and package-lock.json, cd interface, npm install and repeat previous step

!!! info "Ignore DragDropList error"
    This error will show up in the logging
    ```
    node_modules/svelte-dnd-list/DragDropList.svelte (595:3): Error when using sourcemap for reporting an error: Can't resolve original location of error.
    ```
    Just ignore it.

* Check serial output
  
    <img width="350" src="https://github.com/user-attachments/assets/c925d883-173b-4288-89d4-4770c2d86a02" />

* Go to [development](https://moonmodules.org/MoonLight/develop/development/) for further info on developing MoonBase / MoonLight functionality.

### Other troubles
* In general: first close and restart VSCode and run ☑️ or ➡️ again.
* python > 3.10: install python (3.11.13) in the VSCode shell (not in a normal os terminal)
* esptool.py not downloaded: deinstall platformIO IDE and install pioarduino IDE extensions (required for support of latest esp-idf 5.5)
* Run everything from within VSCode, close any alternative / external tools which (potentially) access esp32 devices. They can claim resources or send commands to the device which interfere with what we trying to accomplish here.
* If the ESP32 device AP is not showing up in your WiFi list it might be helpful to fully erase the ESP32 device before flashing (VSCode 👽, Erase flash)
* Sometimes the Serial log may show: [5817][W][WiFiGeneric.cpp:1408] setTxPower(): Neither AP or STA has been started. This is from setTxPower in APSettingsService. Delay has been added to prevent this.

### MoonLight Installer

!!! tip "4.3.2.1"
    The device will show in 'Captive portal' mode. Alternatively you can close the captive portal and show it in a browser using [http://4.3.2.1](http://4.3.2.1)

!!! warning "UI not showing when installing new version of MoonLight"

    If you ran previous versions of MoonLight, the UI might not show up if it has been changed since then. In this case it is needed to reload the UI from the ESP32-device (use [http://4.3.2.1](http://4.3.2.1))

    * Chrome: Command Shift R (Mac) or Ctrl Shift R or Control Reload (Windows)
    * Safari: Reload from Origin : Option Shift R
    * After this not all UI elements might be immediately visible. If you see 'MoonLight loading ...' in the browser, just wait a bit (☕️)

    * Sometimes it migh be needed to clear the browser cache:
        * Chrome: Delete Browsing data / delete data (caching data)
        * Safari: Empty cache: Menu Develop (enable if not visible in the Safari Menu) / empty caches (Command Option E)

        * More info: [how to hard refresh your browser and clear cache](https://fabricdigital.co.nz/blog/how-to-hard-refresh-your-browser-and-clear-cache)

!!! info "Hostname"
    The hostname will be used to access your device. E.g. if the hostname is ml-home you can access it using [http://ml-home.local](http://ml-home.local)

    * Restart the device to make the hostname known to your network (go to system status, scroll down and press restart)
    * Enter the hostname in your browser e.g. [http://ml-home.local](http://ml-home.local)
    * ESP32-P4-Nano: November 16, 2025: hostname is not working correctly, use IP address instead

!!! tip "Choose driver"
    FastLED driver is best to start with if you have a normal (ws2812) LED strip or panel. 

    Choose the Parallel LED Driver if you have more then 4 LED strips or panels or non standard LEDS (e.g. RGBW lights, curtains...). Other drivers (Virtual, Hub,) not supported yet.

    In some cases restart the device to make layout changes effective is needed.


!!! info "Tools used"
    The installer is powered by [ESP Web Tools](https://esphome.github.io/esp-web-tools)
    See also [ESP-Web-Tools-Tutorial](https://github.com/witnessmenow/ESP-Web-Tools-Tutorial/blob/main/README.md)

    [ESPConnect on GitHub](https://github.com/thelastoutpostworkshop/ESPConnect)
    ESPConnect is based on [Jason2866](https://github.com/Jason2866)'s [WebSerial ESPTool](https://github.com/Jason2866/WebSerial_ESPTool/tree/development).

### USB-to-serial chip drivers

In case your computer does not recognise your connected ESP32, you might need to install the right USB-to-serial chip drivers.
Below are the drivers for common chips used in ESP devices. See the images below to see what chip your ESP32 has.

* CP210x drivers: [Windows & Mac](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers)
* CH342, CH343, CH9102 drivers: [Windows](https://www.wch.cn/downloads/CH343SER_ZIP.html), [Mac](https://www.wch.cn/downloads/CH34XSER_MAC_ZIP.html) (download via blue button with download icon)</li>
* CH340, CH341 drivers: [Windows](https://www.wch.cn/downloads/CH341SER_ZIP.html), [Mac](https://www.wch.cn/downloads/CH341SER_MAC_ZIP.html) (download via blue button with download icon)</li>

<img src="https://moonmodules.org/MoonLight/firmware/installer/images/ch9102.jpg" height="200"/>
<img src="https://moonmodules.org/MoonLight/firmware/installer/images/cp210x-ch34x.jpg" height="200"/>
