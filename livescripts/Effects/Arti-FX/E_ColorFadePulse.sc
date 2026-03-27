// E_ColorFadePulse.sc - Color Fade Pulse
// Produces a smooth fading color pulse that travels along the strip.
// @Copyright © 2026 GitHub MoonLight Commit Authors

#define PI 3.14159
#define PI2 6.28318

void loop() {
  float t1 = (millis() % 655) / 655.0;
  float t2 = (millis() % 6553) / 6553.0 * PI2;
  float t3 = (millis() % 1310) / 1310.0;

  for (int i = 0; i < NUM_LEDS; i++) {
    float il = i * 1.0 / NUM_LEDS;
    float h = il * 2.0 - t1;
    float s = (1.0 + sin(t2 + il * PI)) / 2.0;
    float vt = (t3 + il * 4.0);
    float vf = vt - (int)vt;
    float v = vf * 2.0 - 1.0;
    if (v < 0.0) v = 0.0 - v;
    v = 1.0 - v;
    setHSV(i, h * 255, s * 255, v * 255);
  }
}
