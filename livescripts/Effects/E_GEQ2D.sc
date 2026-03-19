// E_GEQ2D.sc - 2D Graphic Equalizer
// Displays audio frequency bands as colored columns

uint8_t fade = 200;

void setup() {
  addControl(&fade, "fade", "slider", 1, 255);
}

void loop() {
  fadeToBlackBy(fade);
  for (int x = 0; x < width; x++) {
    // map column to one of the 16 bands
    uint8_t band = x * 16 / width;
    uint8_t level = bands[band];
    // scale level to column height
    uint8_t barHeight = level * height / 255;
    for (int y = 0; y < barHeight; y++) {
      setRGBXY(x, y, ColorFromPalette(x * 255 / width + y * 4, 255));
    }
  }
}
