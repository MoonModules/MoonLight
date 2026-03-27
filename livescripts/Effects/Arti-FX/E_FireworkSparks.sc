// E_FireworkSparks.sc - Firework Rocket Sparks
// Flickering rocket sparks that travel in a sine wave with random bursts.
// @Copyright © 2026 GitHub MoonLight Commit Authors

#define PI2 6.28318

void loop() {
  float t1 = (millis() % 3276) / 3276.0;

  for (int i = 0; i < NUM_LEDS; i++) {
    float il = i * 1.0 / NUM_LEDS;
    float v = (1.0 + sin((t1 + il) * PI2)) / 2.0;
    float v2 = (1.0 + sin((t1 + (i + 10.0) / NUM_LEDS) * PI2)) / 2.0;
    bool spark = (v2 < 0.95);
    float vout = 0.0;
    if (v > 0.95 && random16(65535) > 59000) vout = v;

    float h;
    if (spark) {
      h = random16(65535) / 65535.0;
    } else {
      float hv = i / 20.0;
      float hf = hv - (int)(hv / 0.2) * 0.2;
      h = hf;
    }

    float sv = 1.0;
    if (spark) sv = 0.0;
    float val = sv + vout;
    if (val > 1.0) val = 1.0;
    setHSV(i, h * 255, sv * 255, val * 255);
  }
}
