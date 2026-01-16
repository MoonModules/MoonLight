void loop() {
  fadeToBlackBy(100);
  int x = millis() / 100;
  for (int y = 0; y < height; y++) {
      setRGB(y*width+x%width, CRGB(255,0,0));
  }
}