// E_Ripple.sc
// Ripple - expanding ripple from random center
// Original: ripple.wled by Andrew Tuline

uint8_t fade = 64;
int step = -1;
int center = 0;
uint8_t colour = 0;
int maxsteps = 16;

void setup() {
  addControl(&fade, "fade", "slider", 1, 255);
}

void loop() {
  fadeToBlackBy(fade);

  if (step < 0) {
    center = random16(NUM_LEDS);
    colour = random16(255);
    step = 0;
  }

  if (step > 0) {
    int bri = 255 / step;
    int ledL = (center + step + NUM_LEDS) % NUM_LEDS;
    int ledR = (center - step + NUM_LEDS) % NUM_LEDS;
    setRGBPal(ledL, colour, bri);
    setRGBPal(ledR, colour, bri);
  }
  step = step + 1;

  if (step >= maxsteps) step = -1;
}
