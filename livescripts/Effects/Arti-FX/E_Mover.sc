// Mover - moving colored segments
// Original: Mover.wled (idea by @Atuline)

void loop() {
  for (int i = 0; i < NUM_LEDS; i++) {
    setRGB(i, CRGB(0, 0, 0));
  }

  int locn = millis() / 100;
  for (int i = 0; i < NUM_LEDS; i = i + 30) {
    int p1 = (locn + i) % NUM_LEDS;
    int p2 = (locn + i + 10) % NUM_LEDS;
    int p3 = (locn + i + 20) % NUM_LEDS;
    setHSV(p1, 50, 255, 255);
    setHSV(p2, 125, 255, 255);
    setHSV(p3, 200, 255, 255);
  }
}
