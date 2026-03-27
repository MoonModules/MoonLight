// E_RainbowFonts2.sc
// Rainbow Fonts 2 - center-outward with oscillating offset
// Original: rainbow_fonts_2.wled (PixelBlaze, converted by Andrew Tuline)

#define PI2 6.28318

void loop() {
  float t1 = (millis() % 6553) / 6553.0;
  int scale = NUM_LEDS / 2;
  float timeOff = (millis() % 13107) / 13107.0;
  float offset = sin(timeOff * PI2) * NUM_LEDS / 10.0;

  for (int i = 0; i < NUM_LEDS; i++) {
    float diff = (i + offset) - scale;
    if (diff < 0) diff = 0.0 - diff;
    float c = 1.0 - diff / scale;
    c = (1.0 + sin(c * PI2)) / 2.0;
    c = (1.0 + sin((c + t1 + offsetL) * PI2)) / 2.0;
    setHSV(i, c * 255, 255, 255);
  }
}
