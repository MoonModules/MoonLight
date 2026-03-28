// L_panel080-048.sc - 5×3 Grid of 16×16 Panels (80×48)
// 15 panels in a 5-wide × 3-tall grid, serpentine wiring, 2 pins (8 panels + 7 panels).
// Panels wired right-to-left; each column uses serpentine (x%2) row order.
// Original: F_panel080-048.sc (StarLight), cross compiled by MoonLight

#define horizontalPanels 5
#define verticalPanels   3
#define panelWidth       16
#define panelHeight      16

void onLayout() {
  int panelCount = 0;
  for (int panelY = 0; panelY < verticalPanels; panelY++) {
    for (int panelX = horizontalPanels - 1; panelX >= 0; panelX--) {
      for (int x = 0; x < panelWidth; x++)
        for (int y = panelHeight - 1; y >= 0; y--)
          addLight(panelX * panelWidth + x,
                   panelY * panelHeight + ((x % 2) ? y : panelHeight - 1 - y), 0);
      panelCount++;
      if (panelCount % 8 == 0) nextPin();
    }
  }
  nextPin(); // remaining panels
}
