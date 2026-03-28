// L_panel4020.sc - 40×20 Panel Layout
// Single flat 40-wide × 20-tall LED panel on one pin.
// Original: F_panel4020.sc (StarLight), cross compiled by MoonLight

#define panelWidth  40
#define panelHeight 20

void onLayout() {
  for (int y = 0; y < panelHeight; y++)
    for (int x = 0; x < panelWidth; x++)
      addLight(x, y, 0);
  nextPin();
}
