// E_Kitt.sc - Kitt
// A colored bar of light bounces back and forth along the strip like the KITT scanner.
// Original: Kitt.wled from MoonModules/MM-Effects, cross compiled by MoonLight

int pixelCounter = 0;
bool goingUp = true;

void loop() {
  if (pixelCounter > NUM_LEDS - 5) goingUp = false;
  if (pixelCounter == 0) goingUp = true;

  setRGBPal(pixelCounter, pixelCounter, 255);

  if (goingUp) {
    if (pixelCounter >= 5) setRGB(pixelCounter - 5, CRGB(0, 0, 0));
    pixelCounter = pixelCounter + 1;
  } else {
    if (pixelCounter + 5 < NUM_LEDS) setRGB(pixelCounter + 5, CRGB(0, 0, 0));
    pixelCounter = pixelCounter - 1;
  }
}
