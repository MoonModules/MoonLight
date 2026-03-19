// Matrix 2D Pulse - pulsing 2D matrix pattern
// Original: matrix_2D_pulse.wled (PixelBlaze, converted by Ewoud Wijma)
// Requires a 2D layout (panel)

#define PI2 6.28318

void loop() {
  float t1 = (millis() % 3276) / 3276.0 * PI2;
  float t2 = (millis() % 5898) / 5898.0 * PI2;
  float tw = (millis() % 13107) / 13107.0;
  float z = 1.0 + (1.0 + sin(tw * PI2)) / 2.0 * 5.0;

  for (int x = 0; x < width; x++) {
    for (int y = 0; y < height; y++) {
      float h = (1.0 + sin(x * 1.0 / width * z + t1) + cos(y * 1.0 / height * z + t2)) * 0.5;
      float v = h;
      v = v * v * v;
      setRGBXY(x, y, hsv(h * 255, 255, v / 2.0 * 255));
    }
  }
}
