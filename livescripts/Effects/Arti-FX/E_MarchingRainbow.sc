// Marching Rainbow - layered sine waves
// Original: marching_rainbow.wled (PixelBlaze, converted by Andrew Tuline)

#define PI2 6.28318

void loop() {
  float t1 = (millis() % 6553) / 6553.0;
  float t2 = (millis() % 3276) / 3276.0;

  for (int i = 0; i < NUM_LEDS; i++) {
    float il = i * 1.0 / NUM_LEDS;
    float w1 = (1.0 + sin((t1 + il) * PI2)) / 2.0;
    float w2 = (1.0 + sin((t2 - il * 10.0 + 0.2) * PI2)) / 2.0;
    float v = w1 - w2;
    if (v < 0.0) v = 0.0;

    float ha = (1.0 + sin((t1 + il) * PI2)) / 2.0;
    float hb = (1.0 + sin(ha * PI2)) / 2.0;
    float h = (1.0 + sin((hb - il) * PI2)) / 2.0;
    setHSV(i, h * 255, 255, v * 255);
  }
}
