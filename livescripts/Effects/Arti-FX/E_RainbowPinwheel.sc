// E_RainbowPinwheel.sc
// Rainbow Pinwheel - scrolling rainbow
// Original: rainbow_pinwheel.wled (PixelBlaze)

#define PI2 6.28318

void loop() {
  float t1 = (millis() % 3276) / 3276.0;
  for (int i = 0; i < NUM_LEDS; i++) {
    float h = (1.0 + sin((t1 + i * 1.0 / NUM_LEDS) * PI2)) / 2.0;
    setHSV(i, h * 255, 255, 255);
  }
}
