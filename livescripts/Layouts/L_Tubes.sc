// L_Tubes.sc — Hanging LED tube fixture layout
// 20 vertical LED tubes randomly distributed in a 2 m × 6 m × 1 m space.
//
// Virtual grid is built in 2:6:1 proportions; scale controls the resolution:
//   total virtual pixels ≈ 12 × scale³   (scale=16 → 32×96×16 ≈ 49 000 px)
//
// Tubes hang downward along the Z axis from a random height near the ceiling.
// Each tube occupies one output pin (via nextPin).
//
// To switch from random to fixed positions, replace the onLayout body with
// explicit addTube() calls, one per tube:
//   addTube(x1, y1, zTop, x1, y1, zTop - tubeLen, pixelsPerTube);

uint8_t numTubes      = 20;
uint8_t pixelsPerTube = 54;
uint8_t pixelDistance = 1;    // physical spacing between LEDs, in cm
uint8_t scale         = 16;   // grid base — increase for finer resolution

void setup() {
    addControl(&numTubes,      "numTubes",      "slider",  1, 40);
    addControl(&pixelsPerTube, "pixelsPerTube", "slider",  4, 100);
    addControl(&pixelDistance, "pixelDistance", "slider",  1, 10);
    addControl(&scale,         "scale",         "slider",  4, 32);
}

void onLayout() {
    // Grid dimensions proportional to physical space (2 m : 6 m : 1 m)
    uint16_t gridX = scale * 2;   // width  (2 m)
    uint16_t gridY = scale * 6;   // depth  (6 m)
    uint16_t gridZ = scale;        // height (1 m)

    // Convert physical tube length to grid units.
    // 1 grid unit in Z = 100 cm / gridZ = 100 / scale  cm
    // tube length in grid units = pixelsPerTube * pixelDistance * scale / 100
    uint16_t tubeLen = pixelsPerTube * pixelDistance * scale / 100;
    if (tubeLen == 0) tubeLen = 1;
    if (tubeLen >= gridZ) tubeLen = gridZ - 1;

    for (uint8_t t = 0; t < numTubes; t++) {
        uint16_t x = random16(gridX);
        uint16_t y = random16(gridY);

        // Random attachment height in the upper third of the space, hang downward
        uint16_t headroom = gridZ / 3;
        if (headroom == 0) headroom = 1;
        uint16_t zTop = gridZ - 1 - random16(headroom);
        uint16_t zBot = (zTop >= tubeLen) ? zTop - tubeLen : 0;

        addTube(x, y, zTop, x, y, zBot, pixelsPerTube);
    }
}
