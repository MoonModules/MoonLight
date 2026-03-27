// E_ColorRandom.sc - Color Random
// Lights random pixels with random palette colors each frame.
// @Copyright © 2026 GitHub MoonLight Commit Authors

void loop() {
  setRGBPal(random16(NUM_LEDS), random16(255), 255);
}
