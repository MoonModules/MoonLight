// E_Drip.sc - Drip
// Simulates a single drip falling with gravity and resetting at the bottom.
// speed: drip gravity/acceleration
// fade: trail fade speed (1=long trail, 255=instant)
// Original: drip.wled by Andrew Tuline, cross compiled by MoonLight

uint8_t speed = 50;
uint8_t fade = 64;
float dripSpd = 0.0;
float dripLocn = 0.0;
uint8_t colr = 1;
uint8_t bri = 64;

void setup() {
  addControl(&speed, "speed", "slider", 1, 255);
  addControl(&fade, "fade", "slider", 1, 255);
  dripLocn = NUM_LEDS - 1;
}

void loop() {
  float grav = speed / 1000.0;
  fadeToBlackBy(fade);

  dripSpd = dripSpd + grav;
  dripLocn = dripLocn - dripSpd;

  if (dripLocn >= 0.0) {
    setRGBPal((int)dripLocn, colr, bri);
  }

  if (dripLocn < 0.0) {
    dripLocn = NUM_LEDS - 1;
    dripSpd = 0.0;
  }
}
