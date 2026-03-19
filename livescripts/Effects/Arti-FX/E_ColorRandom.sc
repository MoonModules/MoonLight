// ColorRandom - random palette colors
// Original: ColorRandom.wled from MoonModules/MM-Effects

void loop() {
  setRGBPal(random16(NUM_LEDS), random16(255), 255);
}
