// Color Twinkles - sparkling color twinkles
// Original: color_twinkles.wled (PixelBlaze, converted by Andrew Tuline)

#define PI2 6.28318

void loop() {
  float t1 = (millis() % 9830) / 9830.0 * PI2;
  float t2 = (millis() % 32767) / 32767.0 * PI2;

  for (int i = 0; i < NUM_LEDS; i++) {
    float aa = i / 3.0 + PI2 * sin(i / 2.0 + t1);
    float a = (1.0 + sin(aa)) / 2.0;
    a = a * a * a * a;
    if (a <= 0.1) a = 0.0;

    float bb = i / 3.0 + PI2 * sin(i / 2.0 + t2);
    float b = sin(bb);
    setHSV(i, b * 255, 255, a * 255);
  }
}
