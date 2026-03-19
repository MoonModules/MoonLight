// Rainbow Fonts - center-outward rainbow waves
// Original: rainbow_fonts.wled (PixelBlaze, converted by Andrew Tuline)

#define PI2 6.28318

void loop() {
  float t1 = (millis() % 6553) / 6553.0;
  int hl = NUM_LEDS / 2;

  for (int i = 0; i < NUM_LEDS; i++) {
    float diff = i - hl;
    if (diff < 0) diff = 0 - diff;
    float c = 1.0 - diff / hl;
    c = (1.0 + sin(c * PI2)) / 2.0;
    c = (1.0 + sin((c + t1) * PI2)) / 2.0;
    setHSV(i, c * 255, 255, 255);
  }
}
