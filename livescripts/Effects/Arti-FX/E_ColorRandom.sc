// E_ColorRandom.sc - Color Random
// Lights random pixels with random palette colors each frame.
// Original: ColorRandom.wled from MoonModules/MM-Effects, cross compiled by MoonLight

void loop() {
  setRGBPal(random16(NUM_LEDS), random16(255), 255);
}
