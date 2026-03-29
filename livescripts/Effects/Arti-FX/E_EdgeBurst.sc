// E_EdgeBurst.sc - Edge Burst
// Produces color bursts that sweep inward from both edges of the strip.
// Original: edge_burst.wled (PixelBlaze, converted by Andrew Tuline), cross compiled by MoonLight

void loop() {
  // t1 = triangle(time(0.1))
  float tval = (millis() % 6553) / 6553.0;
  float tf = tval * 2.0 - 1.0;
  if (tf < 0.0) tf = 0.0 - tf;
  float t1 = 1.0 - tf;

  for (int i = 0; i < NUM_LEDS; i++) {
    float f = i * 1.0 / NUM_LEDS;
    // triangle(f)
    float ft = f * 2.0 - 1.0;
    if (ft < 0.0) ft = 0.0 - ft;
    float trif = 1.0 - ft;

    float edge = trif + t1 * 4.0 - 2.0;
    if (edge < 0.0) edge = 0.0;
    if (edge > 1.0) edge = 1.0;

    // triangle(edge)
    float et = edge * 2.0 - 1.0;
    if (et < 0.0) et = 0.0 - et;
    float v = 1.0 - et;

    float h = edge * edge - 0.2;
    setHSV(i, h * 255, 255, v * 255);
  }
}
