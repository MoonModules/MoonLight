// Clock 2D - analog clock on a 2D matrix
// Original: Clock2D.wled from MoonModules/MM-Effects
// Requires a 2D layout (panel) and NTP for correct time

#define PI2 6.28318

void loop() {
  // clear all
  for (int i = 0; i < NUM_LEDS; i++) {
    setRGB(i, CRGB(0, 0, 0));
  }

  int cx = width / 2;
  int cy = height / 2;
  int radius = cx;
  if (cy < radius) radius = cy;

  // draw 12 hour markers
  for (int h = 0; h < 12; h++) {
    float angle = h * PI2 / 12.0;
    int mx = cx + sin(angle) * (radius - 1);
    int my = cy - cos(angle) * (radius - 1);
    if (mx >= 0 && mx < width && my >= 0 && my < height) {
      setRGBXY(mx, my, ColorFromPalette(225, 128));
    }
  }

  // second hand
  float sa = second * PI2 / 60.0;
  int sx = cx + sin(sa) * (radius - 1);
  int sy = cy - cos(sa) * (radius - 1);
  drawLine(cx, cy, sx, sy, ColorFromPalette(25, 255));

  // minute hand
  float ma = minute * PI2 / 60.0;
  int mx = cx + sin(ma) * (radius - 2);
  int my = cy - cos(ma) * (radius - 2);
  drawLine(cx, cy, mx, my, ColorFromPalette(92, 255));

  // hour hand (shorter)
  float ha = (hour % 12) * PI2 / 12.0 + minute * PI2 / 720.0;
  int hx = cx + sin(ha) * (radius / 2);
  int hy = cy - cos(ha) * (radius / 2);
  drawLine(cx, cy, hx, hy, ColorFromPalette(160, 255));
}
