// FFT Bands - frequency-reactive brightness per band
// Original: fftBrightness.wled by ewowi

void loop() {
  for (int i = 0; i < NUM_LEDS; i++) {
    uint8_t band = i % 16;
    setRGBPal(i, i * 16, bands[band]);
  }
}
