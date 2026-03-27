// L_SE16.sc - ShieldEsp32 16-Pin Layout
// Defines a 16-pin strip layout with optional mirrored and column/row orientation.
// mirroredPins: pair adjacent pins in opposite directions
// pinsAreColumns: true = strips are columns, false = strips are rows
// ledsPerPin: number of LEDs per output pin

bool mirroredPins = false;
bool pinsAreColumns = false;
uint16_t ledsPerPin = 10;

void setup() {
  addControl(&mirroredPins, "mirroredPins", "checkbox");
  addControl(&pinsAreColumns, "pinsAreColumns", "checkbox");
  addControl(&ledsPerPin, "ledsPerPin", "slider", 1, 255);
}

void addStrip(int xposition, int start_y, int stop_y) {
  if (start_y > stop_y) {
    for (int y = start_y; y >= stop_y; y--) {
      if (pinsAreColumns)
        addLight(xposition, y, 0);
      else
        addLight(y, xposition, 0);
    }
  } else {
    for (int y = start_y; y <= stop_y; y++) {
      if (pinsAreColumns)
        addLight(xposition, y, 0);
      else
        addLight(y, xposition, 0);
    }
  }
  nextPin();
}

void onLayout() {
  if (mirroredPins) {
    addStrip(7, ledsPerPin, 2*ledsPerPin-1); addStrip(7, ledsPerPin-1, 0);
    addStrip(6, ledsPerPin, 2*ledsPerPin-1); addStrip(6, ledsPerPin-1, 0);
    addStrip(5, ledsPerPin, 2*ledsPerPin-1); addStrip(5, ledsPerPin-1, 0);
    addStrip(4, ledsPerPin, 2*ledsPerPin-1); addStrip(4, ledsPerPin-1, 0);
    addStrip(3, ledsPerPin, 2*ledsPerPin-1); addStrip(3, ledsPerPin-1, 0);
    addStrip(2, ledsPerPin, 2*ledsPerPin-1); addStrip(2, ledsPerPin-1, 0);
    addStrip(1, ledsPerPin, 2*ledsPerPin-1); addStrip(1, ledsPerPin-1, 0);
    addStrip(0, ledsPerPin, 2*ledsPerPin-1); addStrip(0, ledsPerPin-1, 0);
  } else {
    addStrip(14, 0, ledsPerPin-1);
    addStrip(15, 0, ledsPerPin-1);
    addStrip(12, 0, ledsPerPin-1);
    addStrip(13, 0, ledsPerPin-1);
    addStrip(10, 0, ledsPerPin-1);
    addStrip(11, 0, ledsPerPin-1);
    addStrip(8, 0, ledsPerPin-1);
    addStrip(9, 0, ledsPerPin-1);
    addStrip(6, 0, ledsPerPin-1);
    addStrip(7, 0, ledsPerPin-1);
    addStrip(4, 0, ledsPerPin-1);
    addStrip(5, 0, ledsPerPin-1);
    addStrip(2, 0, ledsPerPin-1);
    addStrip(3, 0, ledsPerPin-1);
    addStrip(0, 0, ledsPerPin-1);
    addStrip(1, 0, ledsPerPin-1);
  }
}
