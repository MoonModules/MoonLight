// E_FFTBands.sc - FFT Bands
// Maps audio frequency band levels to LED brightness with palette coloring.
// @Copyright © 2026 GitHub MoonLight Commit Authors

void loop() {
  for (int i = 0; i < NUM_LEDS; i++) {
    uint8_t band = i % 16;
    setRGBPal(i, i * 16, bands[band]);
  }
}
