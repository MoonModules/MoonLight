// E_Ripple.sc - Ripple
// An expanding ripple spreads outward from a random center point along the strip.
// fade: trail fade speed (1=long trail, 255=instant)
// Original: ripple.wled by Andrew Tuline, cross compiled by MoonLight

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
