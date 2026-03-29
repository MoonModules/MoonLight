// E_PhaseShift.sc - Phase Shift
// Cycles the palette index multiplier back and forth to create a shifting color pattern.
// Original: PhaseShift.wled from MoonModules/MM-Effects, cross compiled by MoonLight

int pixelCounter = 3;
int countAdd = 1;

void loop() {
  pixelCounter = pixelCounter + countAdd;
  if (pixelCounter > 15) countAdd = 0 - 1;
  if (pixelCounter < 3) countAdd = 1;

  for (int i = 0; i < NUM_LEDS; i++) {
    setRGBPal(i, i * pixelCounter, 255);
  }
}
