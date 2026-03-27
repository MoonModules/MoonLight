// E_Millipede.sc
// Millipede - crawling wave pattern
// Original: millipede.wled (PixelBlaze, converted by Andrew Tuline)

#define PI2 6.28318

void loop() {
  float t1 = (millis() % 3276) / 3276.0;
  float t2 = (millis() % 6553) / 6553.0;
  float tl = (millis() % 6553) / 6553.0;

  for (int i = 0; i < NUM_LEDS; i++) {
    float il = i * 1.0 / NUM_LEDS;
    float ha = (i + tl * NUM_LEDS);
    float hb = ((int)ha % NUM_LEDS) * 5.0 / NUM_LEDS;
    float frac = hb - (int)hb;
    float hc = il + (1.0 + sin(t1 * PI2)) / 2.0;
    float h = frac + hc;
    float v = (1.0 + sin((h + t2) * PI2)) / 2.0;
    v = v * v;
    setHSV(i, h * 255, 255, v * 255);
  }
}
