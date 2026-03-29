// E_Opposites.sc - Opposites
// Two opposing sine waves combine and modulate hue to create a dynamic color pattern.
// Original: opposites.wled (PixelBlaze, converted by Andrew Tuline), cross compiled by MoonLight

#define PI2 6.28318

void loop() {
  float t1 = (millis() % 6553) / 6553.0;
  float t2 = (millis() % 13107) / 13107.0;

  for (int i = 0; i < NUM_LEDS; i++) {
    float il = i * 1.0 / NUM_LEDS;
    float w1 = (1.0 + sin((t1 + il) * PI2)) / 2.0;
    float w2 = (1.0 + sin((t2 - il) * PI2)) / 2.0;
    float wsum = (il + w1 + w2);
    float wsf = wsum - (int)wsum;
    float w3 = (1.0 + sin(wsf * PI2)) / 2.0;
    float hf = w3;
    float hm = hf - (int)(hf / 0.3) * 0.3;

    float h;
    if (hm > 0.15) {
      h = hm + t1;
    } else {
      h = hm + 0.5 + t1;
    }

    float v = (w1 + 0.1) * (w2 + 0.1) * (w3 + 0.1);
    v = v * 2.0;
    if (v > 1.0) v = 1.0;
    setHSV(i, h * 255, 255, v * 255);
  }
}
