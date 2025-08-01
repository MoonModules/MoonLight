# MoonLight info

<img width="350" src="https://github.com/user-attachments/assets/f0023a22-ed53-43f6-98bf-6a0869b59db1" />

Shows info about the physical and virtual layer

* **NrOfLights**: the nr of lights defined in the layer
* **Channels per light**: e.g. normal RGB strip/panel is 3 channels per light
* **Chipset**: FastLED chipset defined (for FastLED hardcoded in the firmware ...)
* **FastLED**: FastLED version used (301001 is 3.10.1)
* **FastLED I2S**: Is the I2S driver used: Used on ESP32-S3 boards (temporary, will move to Physical driver), other boards use RMT5
* **Color order**: FastLED color order (for FastLED hardcoded in the firmware ...), will not work on I2S, see [FastLED issue 1966](https://github.com/FastLED/FastLED/issues/1966)
* **Max channels**: Max channels availeble (12288x3 for boards with PSRAM, 4096*3 for other boards -> 12288 / 4096 RGB LEDs)
* **Size**: the outer bounds of the fixture, e.g. for a 16x16 panel it is 16x16x1
* **Layers**: The virtual layers defined (currently only 1)
    * **NrOfLights and size**: virtual layer can differ from the physical layer (.e.g when mirroring it is only half)
    * **Mapping table#**: nr of entries in the mapping table, is the same is nr of virtual pixels
    * **nrOfZeroLights**: the number of lights which are not mapped to a physical pixel (color is stored in the mapping table)
    * **nrOfOneLight**: The number of lights which have a 1:1 mapping between physical and virtual (if no modifier all is phys)
    * **Mapping table indexes**: The number of physical lights which are in a 1:many mapping
    * **nrOfMoreLights**: the number of virtual lights which are in a 1:many mapping
    * **Nodes#**: The number of nodes assigned to a virtual layer (currently all)
