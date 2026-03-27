// E_RainbowMelt.sc - Rainbow Melt
// A melting rainbow pattern that pulses outward from the center with layered sine modulation.
// Original: rainbow_melt.wled (PixelBlaze, converted by Andrew Tuline), cross compiled by MoonLight

#define PI2 6.28318

void loop() {
  float t1 = (millis() % 6553) / 6553.0;
  float t2 = (millis() % 8519) / 8519.0;
  int hl = NUM_LEDS / 2;
  if (hl < 1) hl = 1;

  for (int i = 0; i < NUM_LEDS; i++) {
    float diff = i - hl;
    if (diff < 0) diff = 0 - diff;
    float c1 = 1.0 - diff * 1.0 / hl;
    float c2 = (1.0 + sin(c1 * PI2)) / 2.0;
    float c3 = (1.0 + sin((c2 + t1) * PI2)) / 2.0;
    float v = (1.0 + sin((c3 + t1) * PI2)) / 2.0;
    v = v * v;
    setHSV(i, (c1 + t2) * 255, 255, v * 255);
  }
}
