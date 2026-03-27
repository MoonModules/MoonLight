// E_ColorTwinkleBounce.sc - Color Twinkle Bounce
// Creates twinkling lights that bounce and shift color over time.
// @Copyright © 2026 GitHub MoonLight Commit Authors

#define PI2 6.28318

void loop() {
  float t1 = (millis() % 3276) / 3276.0 * PI2;
  float t2 = (millis() % 3276) / 3276.0 * PI2;
  float tb = (millis() % 6553) / 6553.0;

  for (int i = 0; i < NUM_LEDS; i++) {
    float aa = i / 2.0 + 5.0 * sin(t1);
    float a = (1.0 + sin(aa)) / 2.0;
    float ba = i / 2.0 + 5.0 * sin(t2);
    float b = tb + 1.0 + sin(ba);
    float v = a * a * a;
    setHSV(i, b * 255, 255, v * 255);
  }
}
