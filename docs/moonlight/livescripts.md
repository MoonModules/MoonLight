# Live Scripts module

<img width="396" alt="image" src="https://github.com/user-attachments/assets/953346f2-780d-4c61-8e73-7ba7bd228041" />

Live Scripts let you write custom effects, layouts, and palettes in a simple C-like language. Scripts are compiled on the ESP32 and executed as native code — no PC needed.

!!! info "Hardware requirements"
    Live Scripts require extra memory and are only supported on specific board targets/builds.
    In practice, use the documented Live Scripts board environments (for example ESP32-S3/ESP32-P4, and PSRAM-enabled profiles where explicitly provided).
    Standard ESP32 boards without PSRAM generally do not have enough memory.

## Where Live Scripts are used

Live Scripts are `.sc` files stored on the ESP32 filesystem. They appear in different modules depending on their type:

| Script type | Prefix | Where to select | What it does |
|---|---|---|---|
| **Effect** | `E_` | [Effects module](effects.md) | Animates LEDs each frame |
| **Layout** | `L_` | [Drivers module](drivers.md) | Defines physical light positions and pin assignments |
| **Palette** | `P_` | [Lights Control](lightscontrol.md) palette dropdown | Sets or animates the global color palette |

The **Live Scripts module** itself does not create scripts — it shows all currently running scripts and lets you stop, restart, or delete them.

### Module controls

* **Scripts**: Lists all running Live Scripts with their status (running, halted, errors)
* Press the edit button to stop, start, or delete a script

---

## Important notes

Live Scripts use a C-like language that is still under active development. If you are familiar with C, writing your own scripts should be straightforward.
See the [example scripts](https://github.com/MoonModules/MoonLight/tree/main/livescripts) for inspiration.

There are a few known limitations:

* **Debugging**: Script output is only visible in the serial monitor. If a script is not behaving as expected:
    * Connect your device via USB to your PC.
    * Open [ESP Connect](https://thelastoutpostworkshop.github.io/microcontroller_devkit/espconnect) (not supported on Safari).
    * Click **Connect** and select your device.
    * Click **Serial Monitor**, then **Start**.
    * Script output appears in the window.
* **Inline constructor arguments**: Passing a constructed value directly as a function argument may not work correctly — for example, `setRGB(i, CRGB(0, 0, 255))`. If you see unexpected results, assign the value to a variable first: `CRGB color = CRGB(0, 0, 255); setRGB(i, color);`
* **Heavy loops**: Long-running calculations in `loop()` can trigger a watchdog crash and reboot. If this happens, the device reboots in safe mode so the script can be edited.
* **Type conversions**: The Live Script compiler sometimes requires explicit type casts where standard C would handle them implicitly. If a script produces unexpected values, try adding an explicit cast.

These limitations are expected to be resolved in future releases.

---

## How to run a Live Script

**Step 1**: Go to [MoonBase / File Manager](https://moonmodules.org/MoonLight/moonbase/files/) and navigate to a folder for your scripts. Create a folder if needed (press the second + button):

<img width="300" alt="image" src="https://github.com/user-attachments/assets/85eeccf2-3f0d-4bf2-ba0a-e3407ff05fc2" />

**Step 2**: Create a new file (press the first + button). Name it with a `.sc` extension and enter your code. You can also upload a `.sc` file. Press save:

<img width="300" alt="image" src="https://github.com/user-attachments/assets/3b7eca3c-ae57-43f6-910e-3fc03f6fa380" />

See example scripts on GitHub: [Effects](https://github.com/MoonModules/MoonLight/tree/main/livescripts/Effects), [Layouts](https://github.com/MoonModules/MoonLight/tree/main/livescripts/Layouts), [Palettes](https://github.com/MoonModules/MoonLight/tree/main/livescripts/Palettes).

**Step 3**: Select the script in the appropriate module:

- **Effects/Modifiers**: Go to [Effects](effects.md), add or edit a node (🔥), and pick the `.sc` file from the dropdown
- **Layouts**: Go to [Drivers](drivers.md), add or edit a layout node (🚥), and pick the `.sc` file
- **Palettes**: Go to [Lights Control](lightscontrol.md), open the palette dropdown, and select a `P_*.sc` file from the LiveScript category

<img width="300" alt="image" src="https://github.com/user-attachments/assets/60f99421-aa74-4aa7-805d-05125cc5f222" />

**Step 4**: The script compiles (takes about a second) and starts running. Controls defined in the script appear below the node:

<img width="398" alt="image" src="https://github.com/user-attachments/assets/0ccb7e23-c3cc-4dfa-8d89-9fc86b1ff5f5" />

**Step 5**: To edit a running script, press the ✐ button on the node and expand "Edit &lt;filename&gt;.sc". Edit and save — the script recompiles and restarts automatically. Editing in File Manager also triggers a recompile.

<img width="300" alt="image" src="https://github.com/user-attachments/assets/4552d564-85ea-454f-a6c4-5265146cdcfc" />

**Step 6**: View all running scripts in the [Live Scripts module](https://moonmodules.org/MoonLight/moonlight/livescripts).

---

## Writing scripts

Scripts use a subset of C. You can define global variables and the following special functions:

| Function | When it runs | Used by |
|---|---|---|
| `setup()` | Once at startup | Effects, Layouts, Palettes |
| `loop()` | Every frame, repeatedly | Effects, Palettes |
| `onLayout()` | When the layout is (re)mapped | Layouts |

A script can combine these — for example, an effect with both `setup()` (to create controls) and `loop()` (to animate).

### Supported types

`uint8_t`, `uint16_t`, `uint32_t`, `int`, `float`, `bool`, `void`, `CRGB`

### Predefined types and constants

| Name | Definition | Description |
|---|---|---|
| `NUM_LEDS` | `#define` | Current number of lights, set before compilation |

---

## Available functions

### General

| Function | Description |
|---|---|
| `uint32_t millis()` | Milliseconds since boot |
| `uint32_t now()` | Same as `millis()` (alias) |
| `uint16_t random16(uint16_t max)` | Random number 0 to max |
| `void delay(uint32_t ms)` | Delay in milliseconds |
| `void pinMode(uint8_t pin, uint8_t mode)` | Set GPIO pin mode |
| `void digitalWrite(uint8_t pin, uint8_t value)` | Write to GPIO pin |

### Math and trigonometry

| Function | Description |
|---|---|
| `float sin(float x)` | Sine (radians) |
| `float cos(float x)` | Cosine (radians) |
| `uint8_t sin8(uint8_t x)` | Fast 8-bit sine (0–255 → 0–255) |
| `uint8_t cos8(uint8_t x)` | Fast 8-bit cosine |
| `float atan2(float y, float x)` | Arctangent of y/x |
| `float hypot(float x, float y)` | Hypotenuse (sqrt(x²+y²)) |
| `uint8_t inoise8(uint16_t x, uint16_t y, uint16_t z)` | 8-bit Perlin noise |
| `uint8_t beatsin8(uint16_t bpm, uint8_t lo, uint8_t hi, uint32_t timebase, uint8_t phase)` | BPM-synced sine wave |
| `uint8_t beat8(uint16_t bpm, uint32_t timebase)` | BPM-synced sawtooth wave |
| `uint8_t triangle8(uint8_t x)` | Triangle wave |

### Effect functions (for `E_` scripts)

| Function | Description |
|---|---|
| `void fadeToBlackBy(uint8_t amount)` | Fade all LEDs toward black |
| `CRGB ColorFromPalette(uint8_t index, uint8_t brightness)` | Look up a color from the current global palette (index 0–255) |
| `CRGB getRGB(uint16_t index)` | Read the current color of LED at index |
| `void setRGB(uint16_t index, CRGB color)` | Set LED at index to an RGB color |
| `void setRGBXY(int x, int y, CRGB color)` | Set LED at coordinate (x, y) — runs through modifier chain |
| `void setRGBXYZ(int x, int y, int z, CRGB color)` | Set LED at coordinate (x, y, z) — runs through modifier chain |
| `void setHSV(uint16_t index, uint8_t h, uint8_t s, uint8_t v)` | Set LED at index to an HSV color |
| `void setHSVXY(int x, int y, uint8_t h, uint8_t s, uint8_t v)` | Set LED at coordinate (x, y) to an HSV color |
| `void setRGBPal(uint16_t index, uint8_t palIndex, uint8_t brightness)` | Set LED using the current palette |
| `void setPan(uint16_t index, uint8_t value)` | Set pan channel (moving heads) |
| `void setTilt(uint16_t index, uint8_t value)` | Set tilt channel (moving heads) |
| `void drawLine(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1, CRGB color)` | Draw a 2D line between two points |
| `void drawCircle(int cx, int cy, uint8_t radius, CRGB color)` | Draw a 2D circle outline |

### Layout functions (for `L_` scripts)

| Function | Description |
|---|---|
| `void addLight(uint8_t x, uint8_t y, uint8_t z)` | Add a light at position (x, y, z) |
| `void nextPin()` | Assign following lights to the next GPIO pin |

### Palette functions (for `P_` scripts)

| Function | Description |
|---|---|
| `void setPalEntry(uint8_t index, uint8_t r, uint8_t g, uint8_t b)` | Set palette entry (index 0–15) to an RGB color |
| `void setPalEntryHSV(uint8_t index, uint8_t h, uint8_t s, uint8_t v)` | Set palette entry to an HSV color |

### Control and layout management

| Function | Description |
|---|---|
| `void addControl(void* var, char* name, char* type, uint8_t min = 0, uint8_t max = 255)` | Add a UI control linked to a variable. Types: `"slider"`, `"checkbox"`, `"pin"`. `min`/`max` are optional for `"checkbox"`. |
| `void modifySize()` | Notify that the layout size has changed (modifiers) |

### Available variables

| Variable | Type | Description |
|---|---|---|
| `width` | `uint8_t` | Layout width (x size) |
| `height` | `uint8_t` | Layout height (y size) |
| `depth` | `uint8_t` | Layout depth (z size) |
| `on` | `bool` | Whether the node is currently enabled |
| `leds` | `CRGB*` | Direct access to the LED array (advanced) |
| `bands` | `uint8_t[16]` | Audio frequency band magnitudes (0–255), 16 bands from bass to treble |
| `volume` | `float` | Current audio volume level |
| `gravityX` | `int` | IMU gravity vector X component |
| `gravityY` | `int` | IMU gravity vector Y component |
| `gravityZ` | `int` | IMU gravity vector Z component |
| `hour` | `uint8_t` | Current hour (0–23), requires NTP |
| `minute` | `uint8_t` | Current minute (0–59), requires NTP |
| `second` | `uint8_t` | Current second (0–59), requires NTP |

---

## Example scripts

### Effect: Random pixels

```c
// E_random.sc
void setup() {
  printf("Run Live Script good morning\n");
}
void loop() {
  setRGB(random16(NUM_LEDS), CRGB(0, 0, 255));
}
```

### Effect: Noise

```c
// E_noise.sc
uint8_t speed = 128;
uint8_t scale = 128;

void setup() {
  addControl(&speed, "speed", "slider", 1, 255);
  addControl(&scale, "scale", "slider", 1, 255);
}

void loop() {
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      uint8_t lightHue8 = inoise8(x * scale, y * scale, now() / (16 - speed/16));
      setRGBPal(y*width+x, lightHue8, 255);
    }
  }
}
```

### Effect: Pan/Tilt for moving heads

```c
// E_PanTilt.sc
uint8_t bpm = 30;
uint8_t pan = 175;
uint8_t tilt = 90;
uint8_t range = 20;
bool invert = false;

void setup() {
    addControl(&bpm, "bpm", "slider", 1, 255);
    addControl(&pan, "pan", "slider", 0, 255);
    addControl(&tilt, "tilt", "slider", 0, 255);
    addControl(&range, "range", "slider", 0, 255);
    addControl(&invert, "invert", "checkbox");
}
void loop() {
  for (int x = 0; x < width; x++) {
    setPan(x, beatsin8(bpm, pan-range, pan + range, 0,  (invert && x%2==0)?128:0));
    setTilt(x, beatsin8(bpm, tilt - range, tilt + range, 0,  (invert && x%2==0)?128:0));
  }
}
```

### Layout: LED panel

```c
// L_panel.sc
uint8_t width = 16;
uint8_t height = 16;

void setup() {
  addControl(&width, "width", "slider", 1, 32);
  addControl(&height, "height", "slider", 1, 32);
}

void onLayout() {
  for (int x = 0; x<width; x++)
    for (int y=0; y<height; y++)
      addLight(x,(x%2==0)?y:height - 1 - y,0);
  nextPin();
}
```

### Palette: Fire colors (static)

```c
// P_Fire.sc
void setup() {
  setPalEntry(0,   0,   0,   0);
  setPalEntry(4, 128,   0,   0);
  setPalEntry(8, 255,  64,   0);
  setPalEntry(12,255, 200,  40);
  setPalEntry(15,255, 255, 200);
}
```

### Palette: Shifting hue (animated)

```c
// P_Shift.sc
uint8_t hueShift;

void loop() {
  for (uint8_t i = 0; i < 16; i++)
    setPalEntryHSV(i, hueShift + i * 16, 255, 255);
  hueShift++;
}
```

---

More example scripts are available on GitHub: [Effects](https://github.com/MoonModules/MoonLight/tree/main/livescripts/Effects), [Layouts](https://github.com/MoonModules/MoonLight/tree/main/livescripts/Layouts), [Palettes](https://github.com/MoonModules/MoonLight/tree/main/livescripts/Palettes). To request new functions, send a [ping on Discord](https://discord.com/channels/700041398778331156/1369578126450884608).
