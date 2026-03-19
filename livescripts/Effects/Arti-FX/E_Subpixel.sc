// Subpixel - smooth sub-pixel movement
// Original: Subpixel.wled (idea by @Atuline)

uint8_t intensity = 128;

void setup() {
  addControl(&intensity, "intensity", "slider", 1, 255);
}

void loop() {
  float t = (sin(millis() / 1000.0) + 1.0) / 2.0;
  t = t * NUM_LEDS;
  int reverseSlider = 256 - intensity;
  if (reverseSlider < 1) reverseSlider = 1;

  for (int i = 0; i < NUM_LEDS; i++) {
    float diff = t - i;
    if (diff < 0.0) diff = 0.0 - diff;
    if (diff > 256 / reverseSlider) diff = 256 / reverseSlider;
    int bri = 256 - (int)(diff * reverseSlider);
    if (bri > 255) bri = 255;
    if (bri < 0) bri = 0;
    setHSV(i, 0, 255, bri);
  }
}
