// Bright Pulse - fill LEDs based on audio volume
// Original: BrightPulseSR.wled (idea by @tony)
// Requires an audio driver (WLED Audio or FastLED Audio)

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
