// E_random.sc - Random Blue Pixels
// Sets random pixels to blue each frame for a sparkling effect.

void setup() {
  printf("Run Live Script good morning\n");
}
void loop() {
  setRGB(random16(255), CRGB(0, 0, 255));
}
