// E_SinColors1D.sc - Breathing palette segments
// Divides the strip into alternating on/off bands of 10% each.
// Every "on" band takes one color from the current palette.
// The whole pattern oscillates back and forth using beatsin8, controlled by bpm.
// Amplitude (0-100%) scales how far the pattern shifts — 100 = one full segment width.

uint8_t bpm = 20;
uint8_t amplitude = 100;

void setup() {
    addControl(&bpm, "bpm", "slider", 1, 120);
    addControl(&amplitude, "amplitude", "slider", 0, 255);
}

void loop() {
    // Use uint16_t to avoid 8-bit int overflow when NUM_LEDS >= 128
    uint16_t n = NUM_LEDS;
    uint16_t segLen = n / 10;
    if (segLen < 1) segLen = 1;
    uint16_t period = segLen * 2;   // one "on" segment + one "off" segment

    // Offset slides the pattern back and forth; amplitude scales the travel distance
    uint16_t maxOffset = segLen * amplitude / 100;
    uint8_t offset = beatsin8(bpm, 0, maxOffset, 0, 0);

    CRGB black = CRGB(0, 0, 0);

    for (uint16_t i = 0; i < n; i++) {
        uint16_t pos = (i + offset) % period;
        if ((pos < segLen)) {
            // "on" zone — pick palette color based on which band this pixel belongs to
            uint16_t seg = (i + offset) / period;
            uint8_t palIndex = (seg % 10) * 26;   // spread 10 bands across 0-255
            setRGBPal(i, palIndex, 255);
        } else {
            setRGB(i, black);
        }
    }
}
