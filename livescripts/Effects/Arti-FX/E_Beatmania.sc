// Beatmania - beat-synced moving lights
// Original: beatmania.wled by Andrew Tuline

uint8_t speed = 128;
uint8_t intensity = 128;

void setup() {
  addControl(&speed, "speed", "slider", 1, 255);
  addControl(&intensity, "intensity", "slider", 1, 255);
}

void loop() {
  fadeToBlackBy(intensity / 8);

  uint8_t locn1 = beatsin8(speed / 3 + 1, 0, NUM_LEDS - 1, 0, 0);
  uint8_t locn2 = beatsin8(speed / 4 + 1, 0, NUM_LEDS - 1, 0, 0);
  uint8_t locn3 = beatsin8(speed / 5 + 1, 0, NUM_LEDS / 2 + NUM_LEDS / 3, 0, 0);

  uint8_t colr1 = beatsin8(intensity / 6 + 1, 0, 255, 0, 0);
  uint8_t colr2 = beatsin8(intensity / 7 + 1, 0, 255, 0, 0);

  uint8_t bri1 = beatsin8(intensity / 6 + 1, 32, 255, 0, 0);
  uint8_t bri2 = beatsin8(intensity / 7 + 1, 32, 255, 0, 0);

  int locn12 = (locn1 + locn2) % NUM_LEDS;
  uint8_t colr12 = colr1 + colr2;
  uint8_t bri12 = bri1 + bri2;

  setRGBPal(locn12, colr12, bri12);
  setRGBPal(locn1, colr2, bri1);
  setRGBPal(locn2 % (NUM_LEDS - 1), colr1, bri2);
}
