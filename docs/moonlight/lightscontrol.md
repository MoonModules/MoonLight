# Lights Control module

<img width="320" src="https://github.com/user-attachments/assets/8ca3e8a4-8025-43a4-8746-336ef72fa81d" />

Controls:

* On: lights on or off
* Brightness: brightness of the LEDs when on
* RGB Sliders: control each color separately.
* Palette: Global palette setting used in Effects.
* Presets: Store the current effects and modifiers or retrieve earlier saved presets. 64 slots available:
    * Blue: Empty preset
    * Green: Saved preset 
        * Hover with the mouse to see effects and modifiers in the preset
        * Double click on a saved preset to delete the preset.
    * Red: Selected preset
    * 🚨: Save (💾) or cancel (↻) Save effects first, before storing them as a preset!
    * Note: Presets only stores Effects and Modifiers, not Layers and Drivers.
* Preset loop: loop over presets (seconds per presets)
* Monitor On: sends LED output to the monitor.

Light Controls is the interface to control lights for the UI, but also for all protocols eg. HA, DMX, Hardware buttons, displays etc
e.g. a DMX controller, can control presets, but not individual preset details.
