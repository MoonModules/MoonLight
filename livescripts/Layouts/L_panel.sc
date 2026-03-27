// L_panel.sc - Single Panel Layout
// Defines a snake-wired rectangular LED panel on a single pin.
// width: panel width in pixels
// height: panel height in pixels
// @Copyright © 2026 GitHub MoonLight Commit Authors

uint8_t width = 16;
uint8_t height = 16;

void setup() {
  addControl(&width, "width", "slider", 1, 32);
  addControl(&height, "height", "slider", 1, 32);
}

void onLayout() {
  for (int x = 0; x<width; x++)
    for (int y=0; y<height; y++)
      addLight(x,(x%2==0)?y:height - 1 - y,0);
  nextPin(); // all lights to one pin
}
