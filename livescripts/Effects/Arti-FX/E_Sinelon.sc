// E_Sinelon.sc - Sinelon
// A single colored dot bounces back and forth along the strip driven by a sine wave.
// speed: movement speed of the dot
// fade: trail fade speed (1=long trail, 255=instant)
// Original: Sinelon.wled by Andrew Tuline, cross compiled by MoonLight

uint8_t speed = 128;
uint8_t fade = 64;

void setup() {
  addControl(&speed, "speed", "slider", 1, 255);
  addControl(&fade, "fade", "slider", 1, 255);
}

void loop() {
  fadeToBlackBy(fade);

  float locn = millis() / ((256.0 - speed) * 4.0);
  float newVal = (sin(locn) + 1.0) / 2.0;
  int pos = (int)(newVal * NUM_LEDS);
  if (pos >= NUM_LEDS) pos = NUM_LEDS - 1;
  if (pos < 0) pos = 0;
  setRGBPal(pos, 0, 255);
}
