// E_FastPulse.sc - Fast Pulse
// A sharp pulsing light wave that travels along the strip with color shift.
// @Copyright © 2026 GitHub MoonLight Commit Authors

#define PI2 6.28318

void loop() {
  float t1 = (millis() % 6553) / 6553.0;
  float w = (1.0 + sin(t1 * PI2)) / 2.0;

  for (int i = 0; i < NUM_LEDS; i++) {
    float il = i * 1.0 / NUM_LEDS;
    float x = (2.0 * w + il);
    float frac = x - (int)x;
    // triangle of frac
    float v = frac * 2.0 - 1.0;
    if (v < 0.0) v = 0.0 - v;
    v = 1.0 - v;
    v = v * v * v * v * v;
    uint8_t s = 255;
    if (v >= 0.9) s = 0;
    setHSV(i, t1 * 255, s, v * 255);
  }
}
