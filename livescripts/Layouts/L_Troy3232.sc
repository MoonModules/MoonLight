// L_Troy3232.sc - Troy 32×32 Serpentine Panel
// 32×32 serpentine panel split across two pins at x==16 and x==0.
// Original: F_Troy3232.sc (StarLight), cross compiled by MoonLight

#define panelWidth  32
#define panelHeight 32

void onLayout() {
  for (int x = panelWidth - 1; x >= 0; x--) {
    for (int y = 0; y < panelHeight; y++)
      addLight(x, (x % 2) ? panelHeight - 1 - y : y, 0);

    if (x == 16) nextPin();
    if (x == 0)  nextPin();
  }
}
