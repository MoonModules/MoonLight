// L_panel2040.sc - 20×40 Panel Layout
// Single flat 20-wide × 40-tall LED panel on one pin.
// Original: F_panel2040.sc (StarLight), cross compiled by MoonLight

#define panelWidth  20
#define panelHeight 40

void onLayout() {
  for (int y = 0; y < panelHeight; y++)
    for (int x = 0; x < panelWidth; x++)
      addLight(x, y, 0);
  nextPin();
}
