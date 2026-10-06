// A backlight strip around a wall: one strand clockwise from the top-left corner, its lights spread over the wall's edge pixels, each corner light shared by the two sides it joins.

class BacklightLayout {
  byte wallCols = 64;
  byte wallRows = 32;
  byte across = 30;
  byte down = 15;

  int dimensions() { return 2; }

  string tags() { return "💫"; }

  void defineControls() {
    addControl("wallCols", wallCols, 2, 255); // the wall's width in pixels
    addControl("wallRows", wallRows, 2, 255); // the wall's height in pixels
    addControl("across", across, 2, 255); // strip lights along the top, both corners included
    addControl("down", down, 2, 255); // strip lights along a side, both corners included
  }

  void placeLights() {
    for (int i = 0; i < across - 1; i = i + 1) {
      addLight(i * (wallCols - 1) / (across - 1), 0, 0);
    }
    for (int i = 0; i < down - 1; i = i + 1) {
      addLight(wallCols - 1, i * (wallRows - 1) / (down - 1), 0);
    }
    for (int i = 0; i < across - 1; i = i + 1) {
      addLight(wallCols - 1 - i * (wallCols - 1) / (across - 1), wallRows - 1, 0);
    }
    for (int i = 0; i < down - 1; i = i + 1) {
      addLight(0, wallRows - 1 - i * (wallRows - 1) / (down - 1), 0);
    }
  }
}
