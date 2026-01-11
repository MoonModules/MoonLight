# Layouts

A layout (🚥) defines the positions of lights connected to a MoonLight device.

* The **coordinates** of each light are defined in a 3D coordinate space 
    * Coordinates needs to be specified in the order the lights are wired so MoonLight knows which light is first, which is second etc.
    * For lights in a straight line (1D, e.g. LED strips or a LED bar), specify coordinates as [x,0,0] 
    * For lights in a flat area (2D, e.g. LED matrix), specify coordinates as [x,y,0] 
* **Multiple layout nodes** can be defined which will be mapped in the order of the layouts
* MoonLight will use the layout definition to generate a **mapping** of the effects to a real world light layout. Most simple example is a panel which has a snake layout. The mapping will create a layer for effects where the snake layout is hidden.
* Layouts also assign groups of LEDs to esp32 GPIO pins.

## Layout 🚥 Nodes

Below is a list of Layouts in MoonLight. 
Want to add a a Layout to MoonLight, see [develop](https://moonmodules.org/MoonLight/develop/overview/). See also [Live scripts](https://moonmodules.org/MoonLight/moonlight/livescripts/) to add runtime effects.

| Name | Preview | Controls | Remarks
| ---- | ----- | ---- | ---- |
| Panel | ![Panel](https://github.com/user-attachments/assets/1a69758a-81e3-4f1f-a47e-a242de105c93)| <img width="320" alt="Panel" src="https://github.com/user-attachments/assets/60e6ba73-8956-45bc-9706-581faa17ba16" /> | Defines a 2D panel with width and height<br>Wiring Order (orientation): horizontal (x), vertical (y), depth (z)<br>X++: starts at Top or bottom, Y++: starts left or right<br>snake aka serpentine layout|
| Panels | ![Panels](https://github.com/user-attachments/assets/422b5842-773b-4173-99c5-7b25cd39b176) | <img width="320" alt="Panels" src="https://github.com/user-attachments/assets/ad5a15ea-f3f9-42b9-b8cf-196e7db92249" /> | Panel layout + Wiring order, directions and snake also for each panel |
| Cube | ![Cube](https://github.com/user-attachments/assets/3ece6f28-519e-4ebf-b174-ea75c30e9fbe) | <img width="320" alt="Cube" src="https://github.com/user-attachments/assets/56393baa-3cc3-4c15-b0b2-dc72f25d36d1" /> | Panel layout + depth<br> Z++ starts front or back<br>multidimensional snaking, good luck 😜 |
| Rings | ![Ring](https://github.com/user-attachments/assets/7f60871d-30aa-4ad4-8966-cdc9c035c034) | <img width="320" alt="Rings" src="https://github.com/user-attachments/assets/ee2165aa-cf01-48cd-9310-9cfde871ac33" /> | 241 LEDs in 9 rings |
| Wheel | ![Wheel](https://github.com/user-attachments/assets/52a63203-f955-4345-a97b-edb0b8691fe1) | <img width="320" alt="Wheel" src="https://github.com/user-attachments/assets/7b83e30b-e2e1-49e6-ad80-5b6925b23018" /> | |
| Human Sized Cube | ![HCS](https://github.com/user-attachments/assets/8e475f9d-ab7a-4b5c-835a-e0b4ddc28f0f) | <img width="320" alt="HCS" src="https://github.com/user-attachments/assets/de1eb424-6838-4af4-8652-89a54929bb03" /> | |
| Toronto Bar Cubes | ![cubes](../../media/moonlight/effects/lines.gif) | <img width="320" alt="cubes" src="../../media/moonlight/layouts/CeilingCubesControls.png"/> | |
| Single Column | ![Single column](https://github.com/user-attachments/assets/a88cea0f-9227-4da4-9a43-b944fd8bef97) | <img width="320" alt="Single column" src="https://github.com/user-attachments/assets/9f9918b9-e1ee-43a8-a02d-7f1ee182888b" /> | Choose Single Column for LED strips |
| Single Row | ![Single row](https://github.com/user-attachments/assets/4ba5a3ac-9312-4bac-876d-cfa3dce41215) | <img width="320" alt="Single row" src="https://github.com/user-attachments/assets/70455279-646c-467d-b8e5-492b1aeae0fa" /> | |
| SE16 | ![SE16](https://github.com/user-attachments/assets/45c7bec7-2386-4c42-8f24-5a57b87f0df9) | <img width="320" alt="SE16" src="https://github.com/user-attachments/assets/0efe941a-acf5-4a2c-a7d6-bdfa91574d1a" /> | Layout(s) including pins for Stephan Electronics 16-Pin ESP32-S3 board<br>see below |
| LightCrafter16 | ![LightCrafter16](https://github.com/user-attachments/assets/45c7bec7-2386-4c42-8f24-5a57b87f0df9) | <img width="320" alt="LightCrafter16" src="https://github.com/user-attachments/assets/0efe941a-acf5-4a2c-a7d6-bdfa91574d1a" /> | Layout(s) for Stephan Electronics LightCrafter16 ESP32-S3 board<br>see below |

!!! warning "Choosing pins"

    Choose the right pins with care. See also the IO module to see which pins can in general be used for LEDs (💡). But depending on a specific boards some pins might also be in use already. 

!!! tip "Multiple layouts"
    Single line, single row or panel are suitable layouts to combine into a larger fixture.

### SE16

16-channel LED strip driver by Stephan Electronics

![SE-16p](../firmware/installer/images/esp32-s3-stephanelec-16p.jpg)

* Leds Per Pin: the number of LEDs connected to each pin
* Pins Are Columns: are the LEDs on a pin a row of the effect (width is 1 (or 2) x ledsPerPin). If not set the LEDs are a column (height is 1 (or 2) x ledsPerPin)
* Mirrored Pins: If set it is assumed that LEDs are connected with increasing positions on 8 pins on one side of the board and decreasing positions on the 8 pins of the other side of the board. The resulting size will have a width of 8 and the height (or width) will be 2 * ledsPerPin. If not set, the width will be 16 and the height (or width) = ledsPerPin

### LightCrafter16

16-channel LED strip driver by Stephan Electronics

![SE-16p](../firmware/installer/images/esp32-s3-lightcrafter16.jpg)

* Leds Per Pin: the number of LEDs connected to each pin
* Pins Are Columns: are the LEDs on a pin a row of the effect (width is 1 (or 2) x ledsPerPin). If not set the LEDs are a column (height is 1 (or 2) x ledsPerPin)

X0Y0 position is on the top left when the board is positioned in such a way that the Ethernet connector is on the top left.