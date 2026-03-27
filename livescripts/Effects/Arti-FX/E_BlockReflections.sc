// E_BlockReflections.sc - Block Reflections
// Displays shifting reflected color blocks using modular arithmetic and sine waves.
// @Copyright © 2026 GitHub MoonLight Commit Authors

#define PI2 6.28318

void loop() {
  float t2 = (millis() % 6553) / 6553.0 * PI2;
  float t1 = (millis() % 6553) / 6553.0;
  float t3 = (millis() % 32767) / 32767.0;
  float t4 = (millis() % 13107) / 13107.0 * PI2;

  // triangle(t1)
  float tf = t1 * 2.0 - 1.0;
  if (tf < 0.0) tf = 0.0 - tf;
  float trit1 = 1.0 - tf;

  // triangle(t3)
  float t3f = t3 * 2.0 - 1.0;
  if (t3f < 0.0) t3f = 0.0 - t3f;
  float trit3 = 1.0 - t3f;

  float m = 0.3 + trit1 * 0.2;

  for (int i = 0; i < NUM_LEDS; i++) {
    float h = sin(t2);
    float hh = (i - NUM_LEDS / 2.0) / NUM_LEDS;
    float hhh = trit3 * 10.0 + 4.0 * sin(t4);
    float hmod = hh * hhh;
    float mf = hmod - (int)(hmod / m) * m;
    h = h + mf;

    float vv = h;
    if (vv < 0.0) vv = 0.0 - vv;
    float vm = m;
    if (vm < 0.0) vm = 0.0 - vm;
    float v = (vv + vm + t1);
    float vf = v - (int)v;
    v = vf * vf;
    setHSV(i, h * 255, 255, v * 255);
  }
}
