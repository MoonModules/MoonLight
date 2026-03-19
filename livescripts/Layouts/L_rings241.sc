#define width 16
#define height 16

uint8_t pin = 16;

void setup() {
  addControl(&pin, "pin", "pin", 1, 48);
}

void add(int leds, int radius) {
  for (int i = 0; i<leds; i++) {
    int x = width / 2.0 + ((sin8(255 * i / leds) - 127) / 127.0) * radius / 10.0;
    int y = height / 2.0 + ((cos8(255 * i / leds) - 127) / 127.0) * radius / 10.0;
    addLight(x, y, 0);
    //printf("%d %d %d\n", leds,x,y);
  }
}

void onLayout() {

  add(1, 0);
  add(8, 13);
  add(12, 23);
  add(16, 33);
  add(24, 43);
  add(32, 53);
  add(40, 63);
  add(48, 73);
  add(60, 83);

  nextPin(); // all lights to one pin
}