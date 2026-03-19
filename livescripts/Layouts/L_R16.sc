// Rings16Layout: 16 rings of 24 LEDs arranged in a hexagonal pattern.
// Each ring is assigned to its own pin (16 pins total).
// Ring positions match the Rings16Layout C++ class.

uint8_t scale = 1;

void setup() {
  addControl(&scale, "scale", "slider", 1, 10);
}

void addRing(int cx, int cy) {
  int nrOfLEDs = 24;
  float radius = nrOfLEDs / 6.2832;  // nrOfLEDs / (2 * PI)
  for (int i = 0; i < nrOfLEDs; i++) {
    float angleRad = 3.14159 + 6.2832 * i / nrOfLEDs;  // PI + (2*PI*i)/nrOfLEDs
    int x = scale * cx - scale * sin(angleRad) * radius;
    int y = scale * cy + scale * cos(angleRad) * radius;
    addLight(x, y, 0);
  }
  nextPin();
}

void onLayout() {
  addRing(59, 23);
  addRing(70, 28);
  addRing(59, 10);
  addRing(59, 34);
  addRing(47, 17);
  addRing(59, 46);
  addRing(41,  5);
  addRing(47, 39);
  addRing(35, 17);
  addRing(41, 53);
  addRing(22, 10);
  addRing(35, 39);
  addRing(22, 23);
  addRing(22, 46);
  addRing(10, 28);
  addRing(22, 34);
}
