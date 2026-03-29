// L_Cube202020.sc - 20×20×10 LED Cube
// Defines a 20×20×10 cube, one LED strip per Z layer, 10 pins total.
// Original: F_Cube202020.sc (StarLight), cross compiled by MoonLight

#define cubeWidth  20
#define cubeHeight 20
#define cubeDepth  10

void onLayout() {
  for (int z = 0; z < cubeDepth; z++) {
    for (int x = 0; x < cubeWidth; x++)
      for (int y = 0; y < cubeHeight; y++)
        addLight(x, y, z);
    nextPin();
  }
}
