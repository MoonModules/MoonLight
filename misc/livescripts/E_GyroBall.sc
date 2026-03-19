// E_GyroBall.sc - Gyro Ball demo
// A glowing ball that rolls across the display following gravity (IMU)
// Requires an IMU driver (e.g. MPU-6050) to be active

uint8_t fade = 50;
uint8_t ballSize = 2;
uint8_t trail = 200;

int posX = 0; // fixed-point position (x256)
int posY = 0;
int velX = 0; // velocity
int velY = 0;

void setup() {
  addControl(&fade, "fade", "slider", 1, 255);
  addControl(&ballSize, "size", "slider", 1, 8);
  addControl(&trail, "trail", "slider", 1, 255);
  posX = width * 128;  // center
  posY = height * 128;
}

void loop() {
  fadeToBlackBy(trail);

  // gravity drives acceleration (-32767..32767 -> scale down)
  velX = velX + gravityX / 4096;
  velY = velY - gravityY / 4096;

  // damping
  velX = velX * 240 / 256;
  velY = velY * 240 / 256;

  // update position (fixed-point, /256 for pixel)
  posX = posX + velX;
  posY = posY + velY;

  // bounce off walls
  if (posX < 0) { posX = 0; velX = -velX / 2; }
  if (posY < 0) { posY = 0; velY = -velY / 2; }
  if (posX >= width * 256) { posX = width * 256 - 1; velX = -velX / 2; }
  if (posY >= height * 256) { posY = height * 256 - 1; velY = -velY / 2; }

  int cx = posX / 256;
  int cy = posY / 256;

  // draw ball
  for (int dx = -ballSize; dx <= ballSize; dx++) {
    for (int dy = -ballSize; dy <= ballSize; dy++) {
      int px = cx + dx;
      int py = cy + dy;
      if (px >= 0 && px < width && py >= 0 && py < height) {
        int dist = dx * dx + dy * dy;
        if (dist <= ballSize * ballSize) {
          uint8_t bright = 255 - dist * 255 / (ballSize * ballSize + 1);
          setRGBPal(py * width + px, beat8(10, 0), bright);
        }
      }
    }
  }
}
