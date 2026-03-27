// E_noise.sc - 2D Perlin Noise
// Fills a 2D matrix with animated Perlin noise colored by palette.
// speed: animation speed
// scale: noise zoom level
// @Copyright © 2026 GitHub MoonLight Commit Authors

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
