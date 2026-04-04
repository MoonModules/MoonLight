// E_PaintBrush.sc - Paintbrush
// Audio-reactive animated lines driven by beatsin8 and audio bands.
// Works in 2D (drawLine) and 3D (drawLine3D) based on the layer depth.
// oscillatorOffset: speed of oscillation (1-16)
// numLines: number of simultaneous lines (1-40)
// fadeRate: trail fade speed (0=no fade, 128=fast)
// original idea: @TroyHacks

uint8_t oscillatorOffset = 4;
uint8_t numLines = 20;
uint8_t fadeRate = 40;

void setup() {
  addControl(&oscillatorOffset, "oscillatorOffset", "slider", 1, 16);
  addControl(&numLines, "numLines", "slider", 1, 40);
  addControl(&fadeRate, "fadeRate", "slider", 0, 128);
}

uint8_t hue = 0;

void loop() {
  hue++;
  fadeToBlackBy(fadeRate);

  for (uint8_t i = 0; i < numLines; i++) {
    uint8_t bin  = i * 16 / numLines;
    uint8_t band = bands[bin];

    uint8_t x1 = beatsin8(oscillatorOffset * 1 + band / 16, 0, width - 1,  band, 0);
    uint8_t x2 = beatsin8(oscillatorOffset * 2 + band / 16, 0, width - 1,  band, 0);
    uint8_t y1 = beatsin8(oscillatorOffset * 3 + band / 16, 0, height - 1, band, 0);
    uint8_t y2 = beatsin8(oscillatorOffset * 4 + band / 16, 0, height - 1, band, 0);

    CRGB color = ColorFromPalette(i * 255 / numLines + hue, 255);

    if (depth > 1) {
      uint8_t z1 = beatsin8(oscillatorOffset * 5 + band / 16, 0, depth - 1, band, 0);
      uint8_t z2 = beatsin8(oscillatorOffset * 6 + band / 16, 0, depth - 1, band, 0);
      int dx = (int)x2 - (int)x1;
      int dy = (int)y2 - (int)y1;
      int dz = (int)z2 - (int)z1;
      int dist2 = dx * dx + dy * dy + dz * dz;
      if (dist2 > 1) drawLine3D(x1, y1, z1, x2, y2, z2, color);
    } else {
      int dx = (int)x2 - (int)x1;
      int dy = (int)y2 - (int)y1;
      int dist2 = dx * dx + dy * dy;
      if (dist2 > 1) drawLine(x1, y1, x2, y2, color);
    }
  }
}
