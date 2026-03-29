// L_LC16.sc - LightCrafter16 Board Layout
// Defines a 16-pin snake-wired grid layout for the LightCrafter16 board.
// pinsAreColumns: true = strips are columns, false = strips are rows
// ledsPerPin: number of LEDs per output pin

bool pinsAreColumns = false;
uint16_t ledsPerPin = 10;

void setup() {
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
  addStrip(0, ledsPerPin-1, 0);
  addStrip(1, ledsPerPin-1, 0);
  addStrip(2, ledsPerPin-1, 0);
  addStrip(3, ledsPerPin-1, 0);
  addStrip(4, ledsPerPin-1, 0);
  addStrip(5, ledsPerPin-1, 0);
  addStrip(6, ledsPerPin-1, 0);
  addStrip(7, ledsPerPin-1, 0);
  addStrip(7, ledsPerPin, 2*ledsPerPin-1);
  addStrip(6, ledsPerPin, 2*ledsPerPin-1);
  addStrip(5, ledsPerPin, 2*ledsPerPin-1);
  addStrip(4, ledsPerPin, 2*ledsPerPin-1);
  addStrip(3, ledsPerPin, 2*ledsPerPin-1);
  addStrip(2, ledsPerPin, 2*ledsPerPin-1);
  addStrip(1, ledsPerPin, 2*ledsPerPin-1);
  addStrip(0, ledsPerPin, 2*ledsPerPin-1);
}
