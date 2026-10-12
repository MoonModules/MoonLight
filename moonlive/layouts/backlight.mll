// A backlight strip around a wall: one strand over the wall's edge pixels, each corner light shared by the two sides it joins. `startCorner`, `clockwise` and `offset` say where the strand enters and which way it runs, which reorders the INDICES only: the emitted coordinates are a property of the box and stay the same however the strip is wired.

class BacklightLayout {
  byte wallCols = 64;
  byte wallRows = 32;
  byte across = 30;
  byte down = 15;
  byte startCorner = 0;
  byte clockwise = 1;
  int offset = 0;

  int dimensions() { return 2; }

  string tags() { return "💫"; }

  void defineControls() {
    addControl("wallCols", wallCols, 2, 255); // the wall's width in pixels
    addControl("wallRows", wallRows, 2, 255); // the wall's height in pixels
    addControl("across", across, 2, 255); // strip lights along the top, both corners included
    addControl("down", down, 2, 255); // strip lights along a side, both corners included
    addControl("startCorner", startCorner, 0, 3); // 0 top-left, 1 top-right, 2 bottom-right, 3 bottom-left
    addControl("clockwise", clockwise, 0, 1); // 0 runs the strand the other way round
    addControl("offset", offset, 0, 2000); // lights past startCorner where the strand actually begins
  }

  // The light at canonical walk position `k`: clockwise from the top-left, in four segments, each
  // dropping the corner the previous one emitted.
  void emitAt(int k) {
    int side = across - 1;
    int end = down - 1;
    if (k < side) {
      addLight(k * (wallCols - 1) / side, 0, 0);
    }
    else if (k < side + end) {
      addLight(wallCols - 1, (k - side) * (wallRows - 1) / end, 0);
    }
    else if (k < side + end + side) {
      addLight(wallCols - 1 - (k - side - end) * (wallCols - 1) / side, wallRows - 1, 0);
    }
    else {
      addLight(0, wallRows - 1 - (k - side - end - side) * (wallRows - 1) / end, 0);
    }
  }

  // Where index 0 sits on that walk, per corner: each is the first light of the segment leaving it.
  int startIndex() {
    int side = across - 1;
    int end = down - 1;
    if (startCorner == 1) { return side; }
    else if (startCorner == 2) { return side + end; }
    else if (startCorner == 3) { return side + end + side; }
    return 0;
  }

  void placeLights() {
    int n = 2 * (across - 1) + 2 * (down - 1);
    int s = mod(startIndex() + offset, n);
    for (int i = 0; i < n; i = i + 1) {
      if (clockwise == 1) { emitAt(mod(s + i, n)); }
      else { emitAt(mod(s + n - mod(i, n), n)); }
    }
  }
}
