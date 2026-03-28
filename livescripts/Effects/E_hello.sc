// E_hello.sc - Hello World
// Sweeps a single lit pixel through all LEDs with a configurable RGB color.
// red: red channel (0-255)
// green: green channel (0-255)
// blue: blue channel (0-255)
// Original: E_hello.sc (StarLight), cross compiled by MoonLight

uint8_t red   = 255;
uint8_t green = 0;
uint8_t blue  = 0;

int index;

void setup() {
  addControl(&red,   "red",   "slider", 0, 255);
  addControl(&green, "green", "slider", 0, 255);
  addControl(&blue,  "blue",  "slider", 0, 255);
}

void loop() {
  fadeToBlackBy(2);
  setRGB(index, rgb(red, green, blue));
  index += 1;
  if (index == NUM_LEDS) index = 0;
}
