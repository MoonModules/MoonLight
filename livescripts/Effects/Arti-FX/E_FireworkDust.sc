// Firework Dust - random sparkles
// Original: firework_dust.wled (PixelBlaze)

void loop() {
  for (int i = 0; i < NUM_LEDS; i++) {
    if (random16(65535) > 64500) {
      setHSV(i, random16(255), 255, 255);
    } else {
      setRGB(i, CRGB(0, 0, 0));
    }
  }
}
