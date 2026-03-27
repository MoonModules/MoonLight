// E_BrightPulse.sc - Bright Pulse
// Fills all LEDs with a brightness level driven by audio volume.
// sensitivity: audio sensitivity scaling (higher = brighter response)
// @Copyright © 2026 GitHub MoonLight Commit Authors

uint8_t speed = 128;

void setup() {
  addControl(&speed, "sensitivity", "slider", 1, 255);
}

void loop() {
  int lum = volume * 256 / (256 - speed);
  if (lum > 255) lum = 255;
  if (lum < 0) lum = 0;

  for (int i = 0; i < NUM_LEDS; i++) {
    setHSV(i, 0, 0, lum);
  }
}
