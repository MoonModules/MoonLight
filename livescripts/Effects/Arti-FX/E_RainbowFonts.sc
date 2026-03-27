// E_RainbowFonts.sc - Rainbow Fonts
// Center-outward rainbow waves that ripple symmetrically from the middle of the strip.
// Original: rainbow_fonts.wled (PixelBlaze, converted by Andrew Tuline), cross compiled by MoonLight

#define PI2 6.28318

void loop() {
  float t1 = (millis() % 6553) / 6553.0;
  int hl = NUM_LEDS / 2;
  if (hl < 1) hl = 1;

  for (int i = 0; i < NUM_LEDS; i++) {
    float diff = i - hl;
    if (diff < 0) diff = 0 - diff;
    float c = 1.0 - diff / hl;
    c = (1.0 + sin(c * PI2)) / 2.0;
    c = (1.0 + sin((c + t1) * PI2)) / 2.0;
    setHSV(i, c * 255, 255, 255);
  }
}
