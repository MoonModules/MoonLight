// E_FireworkDust.sc - Firework Dust
// Random sparkles that flash briefly in random colors like firework dust.
// @Copyright © 2026 GitHub MoonLight Commit Authors

void loop() {
  for (int i = 0; i < NUM_LEDS; i++) {
    if (random16(65535) > 64500) {
      setHSV(i, random16(255), 255, 255);
    } else {
      setRGB(i, CRGB(0, 0, 0));
    }
  }
}
