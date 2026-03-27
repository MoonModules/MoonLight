// E_Smiley.sc - Smiley
// Draws a smiley face with eyes, nose, and mouth on a 2D matrix display.
// Original: Smiley.wled from MoonModules/MM-Effects, cross compiled by MoonLight

void loop() {
  fadeToBlackBy(10);
  CRGB eyeColor = CRGB(255, 0, 0);
  CRGB faceColor = CRGB(255, 255, 0);

  // face circle
  int cx = width / 2;
  int cy = height / 2;
  int minDim = width;
  if (height < minDim) minDim = height;
  int radius = minDim * 2 / 5;
  drawCircle(cx, cy, radius, faceColor);

  // left eye
  int leX = width * 35 / 100;
  int leY = height * 32 / 100;
  int reX = width * 65 / 100;
  drawLine(leX, leY, leX + width / 10, leY, eyeColor);

  // right eye
  drawLine(reX - width / 10, leY, reX, leY, eyeColor);

  // nose
  int nX = width / 2;
  drawLine(nX, height * 45 / 100, nX, height * 55 / 100, eyeColor);

  // mouth
  drawLine(width * 35 / 100, height * 68 / 100, width * 65 / 100, height * 68 / 100, eyeColor);
}
