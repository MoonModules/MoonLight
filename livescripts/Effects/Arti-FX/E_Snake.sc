// Snake - moving segment of light
// Original: snake.wled (PixelBlaze)

uint8_t snakeLen = 10;

void setup() {
  addControl(&snakeLen, "length", "slider", 3, 50);
}

void loop() {
  float t1 = (millis() % 6553) / 6553.0;
  float head = t1 * NUM_LEDS;

  for (int i = 0; i < NUM_LEDS; i++) {
    float h = i * 1.0 / NUM_LEDS;
    float offset = head - i + NUM_LEDS;
    int off = (int)offset % NUM_LEDS;
    float v = 1.0 - off * 1.0 / snakeLen;
    if (v < 0.0) v = 0.0;
    setHSV(i, h * 255, 255, v * 255);
  }
}
