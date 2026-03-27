// E_Clock.sc - 1D Clock
// Shows hour, minute, and second as colored dots at positions along the strip.
// Original: Clock.wled from MoonModules/MM-Effects, cross compiled by MoonLight

void loop() {
  fadeToBlackBy(255);  // instant clear

  // hour, minute, second as dots at positions along the strip
  if (hour < NUM_LEDS) setRGBPal(hour, 25, 255);
  if (minute < NUM_LEDS) setRGBPal(minute, 125, 255);
  if (second < NUM_LEDS) setRGBPal(second, 225, 255);
}
