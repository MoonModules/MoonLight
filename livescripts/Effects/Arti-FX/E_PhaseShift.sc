// E_PhaseShift.sc
// PhaseShift - shifting palette pattern
// Original: PhaseShift.wled from MoonModules/MM-Effects

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
