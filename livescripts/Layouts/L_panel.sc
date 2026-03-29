// L_panel.sc - Single Panel Layout
// Defines a rectangular LED panel on a single pin.
// width: panel width in pixels
// height: panel height in pixels
// snake: 1 = alternating columns reverse direction (snake/serpentine wiring)

uint8_t width = 16;
uint8_t height = 16;
bool snake = true;

void setup() {
  addControl(&width, "width", "slider", 1, 32);
  addControl(&height, "height", "slider", 1, 32);
  addControl(&snake, "snake", "checkbox", 0, 1);
}

void onLayout() {
  for (int x = 0; x < width; x++)
    for (int y = 0; y < height; y++)
      addLight(x, (snake && x % 2 == 1) ? height - 1 - y : y, 0);
  nextPin(); // all lights to one pin
}
