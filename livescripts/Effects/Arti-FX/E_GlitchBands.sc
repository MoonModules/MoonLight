// E_GlitchBands.sc - Glitch Bands
// Glitchy shifting color bands driven by modular arithmetic and layered sine waves.
// Original: glitch_bands.wled (PixelBlaze, converted by Andrew Tuline), cross compiled by MoonLight

#define PI2 6.28318

void loop() {
  float t1 = (millis() % 6553) / 6553.0 * PI2;
  float t2 = (millis() % 6553) / 6553.0;
  float t3 = (millis() % 32767) / 32767.0;
  float t4 = (millis() % 13107) / 13107.0 * PI2;
  float t5 = (millis() % 3276) / 3276.0;
  float t6 = (millis() % 1310) / 1310.0;

  // triangle(t2)
  float tf2 = t2 * 2.0 - 1.0;
  if (tf2 < 0.0) tf2 = 0.0 - tf2;
  float trit2 = 1.0 - tf2;

  // triangle(t3)
  float tf3 = t3 * 2.0 - 1.0;
  if (tf3 < 0.0) tf3 = 0.0 - tf3;
  float trit3 = 1.0 - tf3;

  float m = 0.3 + trit2 * 0.2;

  for (int i = 0; i < NUM_LEDS; i++) {
    float il = i * 1.0 / NUM_LEDS;
    float h = sin(t1);
    float h1 = (i - NUM_LEDS / 2.0) / NUM_LEDS;
    float h2 = trit3 * 10.0 + 4.0 * sin(t4);
    float hmod = h1 * h2;
    float mf = hmod - (int)(hmod / m) * m;
    h = h + mf;

    float s1v = (t5 + il * 5.0);
    float s1f = s1v - (int)s1v;
    float s1t = s1f * 2.0 - 1.0;
    if (s1t < 0.0) s1t = 0.0 - s1t;
    float s1 = 1.0 - s1t;
    s1 = s1 * s1;

    float s2v = (t6 - (i - NUM_LEDS) * 1.0 / NUM_LEDS);
    float s2f = s2v - (int)s2v;
    if (s2f < 0.0) s2f = s2f + 1.0;
    float s2t = s2f * 2.0 - 1.0;
    if (s2t < 0.0) s2t = 0.0 - s2t;
    float s2 = 1.0 - s2t;

    float st = s1 * s2;
    float stt = st * 2.0 - 1.0;
    if (stt < 0.0) stt = 0.0 - stt;

    float v = 0.5;
    if (s1 > s2) v = 1.5;
    setHSV(i, h * 255, sst * 255, v * 170);
  }
}
