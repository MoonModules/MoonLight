// E_HalloweenTwinkles.sc - Halloween Color Twinkles
// Orange and purple twinkling pixels that fade in and out using animated sine waves.
// Original: halloween_color_twinkles.wled (PixelBlaze, converted by Andrew Tuline), cross compiled by MoonLight

#define PI2 6.28318

void loop() {
  float t1 = (millis() % 98302) / 98302.0 * PI2;
  float t2 = (millis() % 22937) / 22937.0 * PI2;

  for (int i = 0; i < NUM_LEDS; i++) {
    float ha = sin(i / 2.0 + t1);
    float h = sin(i / 3.0 + PI2 * ha);

    float va = sin(i / 2.0 + t2);
    float wv = (1.0 + sin((i / 3.0 / PI2 + va) * PI2)) / 2.0;
    float v = wv * wv * wv * wv;
    if (v <= 0.1) v = 0.0;

    // triangle(h) for hue
    float ht = h;
    if (ht < 0.0) ht = 0.0 - ht;
    float tri = ht * 2.0 - 1.0;
    if (tri < 0.0) tri = 0.0 - tri;
    tri = 1.0 - tri;

    if (h > 0.0) {
      h = tri * 0.1 + 0.7;
    } else {
      h = tri * 0.05 + 0.02;
    }
    setHSV(i, h * 255, 255, v * 255);
  }
}
