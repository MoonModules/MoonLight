// L_Cube888.sc - 8×8×8 LED Cube
// Defines a 8×8×8 cube, all LEDs on a single pin.
// Original: F_Cube888.sc (StarLight), cross compiled by MoonLight

#define cubeWidth  8
#define cubeHeight 8
#define cubeDepth  8

void onLayout() {
  for (int z = 0; z < cubeDepth; z++)
    for (int y = 0; y < cubeHeight; y++)
      for (int x = 0; x < cubeWidth; x++)
        addLight(x, y, z);
  nextPin();
}
