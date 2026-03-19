// Spin Cycle - spinning color pattern
// Original: spin_cycle.wled (PixelBlaze, converted by Andrew Tuline)

#define PI2 6.28318

void loop() {
  float t1 = (millis() % 6553) / 6553.0;
  float t2 = (millis() % 6553) / 6553.0;
  float w1 = (1.0 + sin(t1 * PI2)) / 2.0;
  float w2 = (1.0 + sin(t2 * PI2)) / 2.0;

  for (int i = 0; i < NUM_LEDS; i++) {
    float il = i * 1.0 / NUM_LEDS;
    float h = il * (5.0 + w1 * 5.0) + w2 * 2.0;
    float hf = h - (int)h;
    float hmod = hf - (int)(hf / 0.5) * 0.5;
    h = hmod + t1;

    float tv = (il * 5.0 + t1 * 10.0);
    float tvf = tv - (int)tv;
    float v = tvf * 2.0 - 1.0;
    if (v < 0.0) v = 0.0 - v;
    v = 1.0 - v;
    v = v * v * v;
    setHSV(i, h * 255, 255, v * 255);
  }
}
