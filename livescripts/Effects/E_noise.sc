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
      // leds.setRGBColor(leds.XY(x, y), ColorFromPalette(leds.palette, lightHue8));
      setRGBPal(y*width+x, lightHue8, 255);
    }
  }
}