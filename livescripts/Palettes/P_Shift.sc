// P_Shift.sc - Shifting Rainbow Palette
// Animates a full-spectrum HSV rainbow palette that shifts hue over time.
// @Copyright © 2026 GitHub MoonLight Commit Authors

uint8_t hueShift;

void loop() {
  for (uint8_t i = 0; i < 16; i++)
    setPalEntryHSV(i, hueShift + i * 16, 255, 255);
  hueShift++;
}
