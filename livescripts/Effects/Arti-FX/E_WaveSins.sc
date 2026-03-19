// Wave Sins - phase-shifted beat sine waves
// Original: WaveSins.wled by Andrew Tuline

uint8_t speed = 128;
uint8_t intensity = 128;
uint8_t custom1 = 0;
uint8_t custom2 = 128;
uint8_t custom3 = 16;

void setup() {
  addControl(&speed, "speed", "slider", 1, 255);
  addControl(&intensity, "intensity", "slider", 1, 255);
  addControl(&custom1, "offset", "slider", 0, 255);
  addControl(&custom2, "range", "slider", 1, 255);
  addControl(&custom3, "phase", "slider", 1, 255);
}

void loop() {
  for (int i = 0; i < NUM_LEDS; i++) {
    float bri = sin(millis() / 4.0 + i * intensity) * 128.0 + 128.0;
    uint8_t palIdx = beatsin8(speed, custom1, custom1 + custom2, 0, i * custom3);
    setRGBPal(i, palIdx, (int)bri);
  }
}
