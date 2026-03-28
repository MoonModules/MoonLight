// L_panel128-096.sc - 8×6 Grid of 16×16 Panels (128×96)
// 48 panels in an 8-wide × 6-tall grid, serpentine wiring, 6 pins (one per panel row).
// Panels wired right-to-left; each column uses serpentine (x%2) row order.
// Original: F_panel128-096.sc (StarLight), cross compiled by MoonLight

#define horizontalPanels 8
#define verticalPanels   6
#define panelWidth       16
#define panelHeight      16

void onLayout() {
  for (int panelY = 0; panelY < verticalPanels; panelY++) {
    for (int panelX = horizontalPanels - 1; panelX >= 0; panelX--)
      for (int x = 0; x < panelWidth; x++)
        for (int y = panelHeight - 1; y >= 0; y--)
          addLight(panelX * panelWidth + x,
                   panelY * panelHeight + ((x % 2) ? y : panelHeight - 1 - y), 0);
    nextPin(); // one pin per row of panels
  }
}
