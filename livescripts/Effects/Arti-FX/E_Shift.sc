// Shift - shift all pixels and add random at start
// Original: Shift.wled (idea by @Haribro)

void loop() {
  // shift all pixels up by one
  for (int i = NUM_LEDS - 1; i > 0; i--) {
    setRGB(i, getRGB(i - 1));
  }
  setRGBPal(0, random16(255), 255);
}
