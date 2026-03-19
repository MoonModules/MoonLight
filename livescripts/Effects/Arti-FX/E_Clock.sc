// Clock - show hour, minute, second as colored pixels
// Original: Clock.wled from MoonModules/MM-Effects
// Requires NTP to be configured for correct time

void loop() {
  fadeToBlackBy(255);  // instant clear

  // hour, minute, second as dots at positions along the strip
  if (hour < NUM_LEDS) setRGBPal(hour, 25, 255);
  if (minute < NUM_LEDS) setRGBPal(minute, 125, 255);
  if (second < NUM_LEDS) setRGBPal(second, 225, 255);
}
