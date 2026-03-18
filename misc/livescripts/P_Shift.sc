// Dynamic (with loop(), animates palette over time):
// P_Shift.sc

uint8_t hueShift;

void loop() {
  for (uint8_t i = 0; i < 16; i++)
    setPalEntryHSV(i, hueShift + i * 16, 255, 255);
  hueShift++;
}