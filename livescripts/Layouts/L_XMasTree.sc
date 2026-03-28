// L_XMasTree.sc - Christmas Tree 3D Layout
// Arranges LEDs in concentric rings of increasing radius, forming a cone/tree shape.
// Original: F_XMasTree.sc (StarLight), cross compiled by MoonLight

#define PI 3.141592654

void onLayout() {
  for (int radius = 0; radius < 200; radius = radius + 10) {
    if (radius == 0) { addLight(100, 100, 0); continue; }
    for (int i = 0; i < radius; i++) {
      int x = radius + sin(i / (float)radius * 2 * PI) * radius;
      int y = radius + cos(i / (float)radius * 2 * PI) * radius;
      addLight(x / 2, y / 2, radius);
    }
  }
  nextPin();
}
