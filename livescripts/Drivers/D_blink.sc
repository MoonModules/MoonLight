// D_blink.sc - GPIO Blink
// Blinks a GPIO pin on and off with a 1-second period.
// Original: L_blink.sc (StarLight), cross compiled by MoonLight

#define OUTPUT 0x03
#define LOW    0x0
#define HIGH   0x1

uint8_t blinkPin = 2;

void setup() {
  addControl(&blinkPin, "blinkPin", "pin");
  pinMode(blinkPin, OUTPUT);
}

void loop() {
  digitalWrite(blinkPin, HIGH);
  delay(1000);
  digitalWrite(blinkPin, LOW);
  delay(1000);
}
