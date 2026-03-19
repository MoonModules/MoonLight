// Twinkle Up - fade LEDs in and out with pseudo-random timing
// Original: twinkleup.wled by @Atuline

void loop() {
  for (int i = 0; i < NUM_LEDS; i++) {
    // pseudo-random per-pixel values using sin8 as hash
    uint8_t startVal = sin8(i * 137 + 73);
    uint8_t freq = 255 - sin8(i * 53 + 17) / 16 - 16;
    if (freq < 1) freq = 1;
    float pixbri = (sin(startVal + millis() * 1.0 / freq) + 1.0) * 128.0;
    uint8_t palIdx = sin8(i * 89 + 31);
    setRGBPal(i, palIdx + millis() / 100, (int)pixbri);
  }
}
