uint8_t bpm = 30;
uint8_t pan = 175;
uint8_t tilt = 90;
uint8_t range = 20;
bool invert = false;

void setup() {
    addControl(&bpm, "bpm", "slider", 1, 255);
    addControl(&pan, "pan", "slider", 0, 255);
    addControl(&tilt, "tilt", "slider", 0, 255);
    addControl(&range, "range", "slider", 0, 255);
    addControl(&invert, "invert", "checkbox");
}
void loop() {
  for (int x = 0; x < width; x++) {
    setPan(x, beatsin8(bpm, pan-range, pan + range, 0,  (invert && x%2==0)?128:0));
    setTilt(x, beatsin8(bpm, tilt - range, tilt + range, 0,  (invert && x%2==0)?128:0));
  }
}