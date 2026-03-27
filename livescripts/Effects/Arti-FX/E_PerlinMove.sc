// E_PerlinMove.sc - Perlin Move
// Perlin noise drives multiple pixels to random positions with palette coloring.
// speed: movement speed of noise-driven pixels
// intensity: number of simultaneously moving pixels
// fade: trail fade speed (1=long trail, 255=instant)
// Original: PerlinMove.wled from MoonModules/MM-Effects, cross compiled by MoonLight

uint8_t speed = 128;
uint8_t intensity = 128;
uint8_t fade = 200;

void setup() {
  addControl(&speed, "speed", "slider", 1, 255);
  addControl(&intensity, "intensity", "slider", 1, 255);
  addControl(&fade, "fade", "slider", 1, 255);
}

void loop() {
  fadeToBlackBy(fade);

  for (int i = 0; i < intensity / 16 + 1; i++) {
    uint16_t y = millis() * 128 / (260 - speed);
    uint16_t x = y + i * 200;
    uint8_t locn = inoise8(x, y, 0);
    uint16_t x2 = millis() * 2 + i * 200;
    uint8_t clr = inoise8(x2, 0, 0);

    int pixloc = locn * NUM_LEDS / 256;
    if (pixloc >= NUM_LEDS) pixloc = NUM_LEDS - 1;
    if (pixloc < 0) pixloc = 0;

    setRGBPal(pixloc, clr, 255);
  }
}
