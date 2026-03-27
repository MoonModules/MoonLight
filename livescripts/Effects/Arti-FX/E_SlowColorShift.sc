// E_SlowColorShift.sc
// Slow Color Shift - gently shifting colors
// Original: slow_color_shift.wled (PixelBlaze, converted by Andrew Tuline)

#define PI2 6.28318

void loop() {
  float t1 = (millis() % 9830) / 9830.0 * PI2;
  float t2 = (millis() % 6553) / 6553.0;
  int h4 = NUM_LEDS * 4;

  for (int i = 0; i < NUM_LEDS; i++) {
    float il = i * 1.0 / NUM_LEDS;
    float a1 = i / 2.0 + 5.0 * sin(t1);
    float a = (1.0 + sin(a1)) / 2.0;
    float b = (t2 + 1.0 + sin(a1) / 5.0) + i * 1.0 / h4;
    float v = a * a * a * a;
    setHSV(i, b * 255, 255, v * 255);
  }
}
