// L_Ring.sc - Circular Ring Layout
// Places LEDs evenly around a circle using trigonometry.
// Original: F_Ring.sc (StarLight), cross compiled by MoonLight

#define PI       3.141592654
#define radius   200
#define ledCount 100

void onLayout() {
  for (int i = 0; i < ledCount; i++) {
    int x = radius + sin(i / (float)ledCount * 2 * PI) * radius;
    int y = radius + cos(i / (float)ledCount * 2 * PI) * radius;
    addLight(x, y, 0);
  }
  nextPin();
}
