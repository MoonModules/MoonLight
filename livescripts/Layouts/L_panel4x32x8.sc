// L_panel4x32x8.sc - 4 Stacked 32×8 Panels
// 4 vertically stacked 32×8 panels, one pin per row of panels, serpentine wiring.
// Original: F_panel4x32x8.sc (StarLight), cross compiled by MoonLight

#define horizontalPanels 1
#define verticalPanels   4
#define panelWidth       32
#define panelHeight      8

void onLayout() {
  for (int panelY = verticalPanels - 1; panelY >= 0; panelY--) {
    for (int panelX = 0; panelX < horizontalPanels; panelX++)
      for (int x = panelWidth - 1; x >= 0; x--)
        for (int y = 0; y < panelHeight; y++)
          addLight(panelX * panelWidth + x,
                   panelY * panelHeight + ((x % 2) ? y : panelHeight - 1 - y), 0);
    nextPin();
  }
}
