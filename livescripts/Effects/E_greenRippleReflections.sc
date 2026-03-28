// E_greenRippleReflections.sc - Green Ripple Reflections
// 2D ripple effect combining two sine waves and a triangle wave, fixed green hue.
// Note: uses time() and triangle() — requires Arti-FX / PixelBlaze compatible engine.
// Original: green_ripple_reflections.wled (PixelBlaze via Andrew Tuline / Ewoud Wijma), cross compiled by MoonLight

#define PI2  6.28318
#define PI6  18.84956
#define PI10 31.41590

float t1;
float t2;
float t3;

void setup() {
}

void loop() {
  t1 = time(0.03) * PI2;
  t2 = time(0.05) * PI2;
  t3 = time(0.04) * PI2;

  for (uint8_t index = 0; index < width; index++) {
    float a = sin(index * PI10 / width + t1);
    a = a * a;
    float b = sin(index * PI6 / width - t2);
    float c = triangle(index * 3.0 / width + 1 + sin(t3) / 2);
    float v = (a + b + c) / 3;
    v = v * v;

    for (uint8_t y = 0; y < height; y++)
      setRGB(y * width + index, hsv((int)(0.3 * 255), (int)(a * 255), (int)(v * 255)));
  }
}
