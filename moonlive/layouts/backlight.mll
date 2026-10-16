// A backlight strand around a wall, its lights spread over the wall's edge pixels. Hyperion's shape, so a layout copied from there carries over: a count per edge, a gap, and the strand's entry and direction.

class BacklightLayout {
  byte wallCols = 64;
  byte wallRows = 32;
  byte top = 30;
  byte right = 15;
  byte bottom = 30;
  byte left = 15;
  int gapStart = 0;
  int gapLength = 0;
  byte startCorner = 0;
  int position = 0;
  byte reverse = 0;

  int dimensions() { return 2; }

  string tags() { return "💫"; }

  void defineControls() {
    addControl("wallCols", wallCols, 2, 255); // the wall's width in pixels
    addControl("wallRows", wallRows, 2, 255); // the wall's height in pixels
    addControl("top", top, 2, 255); // strand lights along the top, both corners included
    addControl("right", right, 2, 255); // and along the right
    addControl("bottom", bottom, 2, 255); // and along the bottom
    addControl("left", left, 2, 255); // and along the left
    addControl("gapLength", gapLength, 0, 2000); // lights the strand does not have, for the stretch a TV stand covers
    addControl("gapStart", gapStart, 0, 2000); // where that gap begins, counted round from the top-left corner
    addSelect("startCorner", startCorner, "TL|TR|BR|BL"); // the corner the strand enters at, clockwise from top-left
    addControl("position", position, 0, 2000); // lights along the strand, past startCorner, where it actually begins
    addControl("reverse", reverse, 0, 1); // 1 runs the strand the other way round
  }

  // One step along an edge of `count` lights spanning `span` pixels. A single light sits at the
  // start corner, where the span would divide by zero.
  int along(int i, int count, int span) {
    if (count <= 1) { return 0; }
    return i * span / (count - 1);
  }

  // The light at perimeter position `k`: clockwise from the top-left, each edge one short of its
  // own count, since the next edge opens with the corner they share.
  void emitAt(int k) {
    int j = k;
    if (j < top - 1) {
      addLight(along(j, top, wallCols - 1), 0, 0);
      return;
    }
    j = j - (top - 1);
    if (j < right - 1) {
      addLight(wallCols - 1, along(j, right, wallRows - 1), 0);
      return;
    }
    j = j - (right - 1);
    if (j < bottom - 1) {
      addLight(wallCols - 1 - along(j, bottom, wallCols - 1), wallRows - 1, 0);
      return;
    }
    j = j - (bottom - 1);
    addLight(0, wallRows - 1 - along(j, left, wallRows - 1), 0);
  }

  // The gap is a stretch of the frame the strand skips, so a strand position past it sits that many
  // lights further round. This is what keeps the walk a plain rotation with a gap in it.
  int perimeterAt(int i) {
    if (gapLength > 0 && i >= gapStart) { return i + gapLength; }
    return i;
  }

  // Where index 0 sits, per corner, in STRAND positions: the corner's place on the frame, less the
  // gap if the gap lies before it.
  int strandStart() {
    int k = 0;
    if (startCorner == 1) { k = top - 1; }
    else if (startCorner == 2) { k = top - 1 + right - 1; }
    else if (startCorner == 3) { k = top - 1 + right - 1 + bottom - 1; }
    if (gapLength > 0 && k >= gapStart + gapLength) { return k - gapLength; }
    if (gapLength > 0 && k >= gapStart) { return gapStart; }
    return k;
  }

  void placeLights() {
    int full = top - 1 + right - 1 + bottom - 1 + left - 1;
    int n = full - gapLength;
    if (gapLength < 1) { n = full; }
    if (n < 1) { return; }
    // Along the STRAND, not around the frame: on a reversed strand the entry moves the other way too.
    int s = mod(strandStart() + position, n);
    if (reverse == 1) { s = mod(strandStart() - position + n + n, n); }
    for (int i = 0; i < n; i = i + 1) {
      if (reverse == 1) { emitAt(perimeterAt(mod(s + n - mod(i, n), n))); }
      else { emitAt(perimeterAt(mod(s + i, n))); }
    }
  }
}
