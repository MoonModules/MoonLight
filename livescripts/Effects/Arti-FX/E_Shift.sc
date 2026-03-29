// E_Shift.sc - Shift
// Shifts all pixels along the strip each frame and inserts a new random color at the start.
// Original: Shift.wled (idea by @Haribro), cross compiled by MoonLight

void loop() {
  // shift all pixels up by one
  for (int i = NUM_LEDS - 1; i > 0; i--) {
    setRGB(i, getRGB(i - 1));
  }
  setRGBPal(0, random16(255), 255);
}
