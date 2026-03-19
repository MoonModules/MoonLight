// Green Ripple Reflections - layered green sine waves
// Original: green_ripple_reflections.wled (PixelBlaze, converted by Andrew Tuline)

#define PI2 6.28318
#define PI6 18.84954
#define PI10 31.4159

void loop() {
  float t1 = (millis() % 1966) / 1966.0 * PI2;
  float t2 = (millis() % 3276) / 3276.0 * PI2;
  float t3 = (millis() % 2621) / 2621.0 * PI2;

  for (int i = 0; i < NUM_LEDS; i++) {
    float il = i * 1.0 / NUM_LEDS;
    float a = sin(il * PI10 + t1);
    a = a * a;
    float b = sin(il * PI6 - t2);
    float c0 = il * 3.0 + 1.0 + sin(t3);
    float cf = c0 / 2.0;
    float cfrac = cf - (int)cf;
    float ct = cfrac * 2.0 - 1.0;
    if (ct < 0.0) ct = 0.0 - ct;
    float c = 1.0 - ct;
    float v = (a + b + c) / 3.0;
    v = v * v;
    setHSV(i, 76, a * 255, v * 255);
  }
}
